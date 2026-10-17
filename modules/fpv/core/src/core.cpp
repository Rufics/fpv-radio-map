#include "core.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

namespace fpv::core {
namespace {

constexpr const char* kId = "fpv.core";

std::string num(double v, int decimals) { return fixed(v, decimals); }

double altitudeOf(const RoutePoint& p, const IWorld& world, AltitudeMode mode) {
    return mode == AltitudeMode::Absolute ? world.groundMsl(p.position) + p.aglM : p.aglM;
}

double altitudeOf(const Waypoint& w, const IWorld& world, AltitudeMode mode) {
    return mode == AltitudeMode::Absolute ? world.groundMsl(w.position) + w.aglM : w.aglM;
}

std::string kmlCoord(const LatLon& p, double alt) { return num(p.lon, 9) + "," + num(p.lat, 9) + "," + num(alt, 3); }

std::string latLonAttrs(const LatLon& p) { return "lat=\"" + num(p.lat, 9) + "\" lon=\"" + num(p.lon, 9) + "\""; }

bool sameGeometry(const Mission& a, const Mission& b) {
    auto same = [](const Waypoint& x, const Waypoint& y) {
        return x.position.lat == y.position.lat && x.position.lon == y.position.lon && x.aglM == y.aglM;
    };
    if (a.returnToPilot != b.returnToPilot || a.waypoints.size() != b.waypoints.size() || !same(a.pilot, b.pilot)) return false;
    for (std::size_t i = 0; i < a.waypoints.size(); ++i) {
        if (!same(a.waypoints[i], b.waypoints[i])) return false;
    }
    return true;
}

json compare(const json& missionA, const json& compareOpt, const IWorld& world, double stepM,
             const std::function<std::unique_ptr<ILinkEvaluator>(const json&)>& evalFor, json& reasons) {
    json missionB = missionA;
    missionB.merge_patch(compareOpt.at("patch"));
    const std::string name = compareOpt.value("name", "B");
    const Mission a = Mission::fromJson(missionA);
    const Mission b = Mission::fromJson(missionB);
    const Route routeA(a, world), routeB(b, world);
    const std::vector<RoutePoint> samplesA = routeSamples(routeA, stepM);
    const std::vector<RoutePoint> samplesB = routeSamples(routeB, stepM);
    const auto evalA = evalFor(missionA);
    const auto evalB = evalFor(missionB);
    const End groundA{a.pilot.position, a.pilot.aglM}, groundB{b.pilot.position, b.pilot.aglM};
    const bool aligned = sameGeometry(a, b);

    std::vector<std::string> links;
    const std::vector<std::string> lb = b.links();
    for (const std::string& l : a.links()) {
        if (std::find(lb.begin(), lb.end(), l) != lb.end()) links.push_back(l);
    }

    json out = {{"name", name}, {"mission", missionB}, {"aligned", aligned}, {"stepM", stepM}, {"links", json::object()}};
    if (!aligned) {
        out["notes"] = {"The two scenarios fly different routes, so only their worst margins are compared, "
                        "not point by point."};
    }
    for (const std::string& link : links) {
        json l = json::object();
        WorstMargin wa, wb;
        json points = json::array();
        std::size_t better = 0, worse = 0;
        for (std::size_t i = 0; i < samplesA.size(); ++i) {
            const double m = evalA->marginDb(link, groundA, {samplesA[i].position, samplesA[i].aglM});
            if (m < wa.marginDb) wa = {m, link, i};
            if (aligned) points.push_back({{"lat", samplesA[i].position.lat}, {"lon", samplesA[i].position.lon},
                                           {"aMarginDb", m}});
        }
        for (std::size_t i = 0; i < samplesB.size(); ++i) {
            const double m = evalB->marginDb(link, groundB, {samplesB[i].position, samplesB[i].aglM});
            if (m < wb.marginDb) wb = {m, link, i};
            if (aligned) {
                const double d = m - points[i].at("aMarginDb").get<double>();
                points[i]["bMarginDb"] = m;
                points[i]["deltaDb"] = d;
                if (d > 0.0) ++better;
                if (d < 0.0) ++worse;
            }
        }
        l["a"] = {{"worstMarginDb", wa.marginDb}, {"sample", wa.sample}};
        l["b"] = {{"worstMarginDb", wb.marginDb}, {"sample", wb.sample}};
        l["deltaWorstDb"] = wb.marginDb - wa.marginDb;
        if (aligned) {
            l["points"] = points;
            l["pointsBetter"] = better;
            l["pointsWorse"] = worse;
        }
        out["links"][link] = l;
        reasons.push_back(reason(kId, "info",
                                 "Scenario " + name + ", " + link + ": worst margin " + num(wb.marginDb, 1) + " dB against " +
                                     num(wa.marginDb, 1) + " dB now (" + (wb.marginDb >= wa.marginDb ? "+" : "") +
                                     num(wb.marginDb - wa.marginDb, 1) + " dB)."));
    }
    return out;
}

}  // namespace

std::string xmlEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c;
        }
    }
    return out;
}

std::vector<RoutePoint> exportVertices(const Route& route, double stepM) {
    if (!(stepM > 0.0)) throw rfmod::BadRequest("export stepM must be positive");
    std::vector<RoutePoint> out;
    for (const Leg& leg : route.legs()) {
        const RoutePoint start = route.at(leg.startM);
        out.push_back({leg.from.position, leg.from.aglM, leg.index, start.courseDeg});
        for (std::size_t i = 1; static_cast<double>(i) * stepM < leg.lengthM; ++i) {
            out.push_back(route.at(leg.startM + static_cast<double>(i) * stepM));
        }
    }
    if (!route.legs().empty()) {
        const Leg& last = route.legs().back();
        out.push_back({last.to.position, last.to.aglM, last.index, route.at(route.length()).courseDeg});
    }
    return out;
}

std::string toKml(const Mission& mission, const Route& route, const IWorld& world, const ExportSettings& s) {
    const std::string mode = s.altitude == AltitudeMode::Absolute ? "absolute" : "relativeToGround";
    auto placemark = [&](const Waypoint& w) {
        return "<Placemark><name>" + xmlEscape(w.label) + "</name><Point><altitudeMode>" + mode +
               "</altitudeMode><coordinates>" + kmlCoord(w.position, altitudeOf(w, world, s.altitude)) +
               "</coordinates></Point></Placemark>\n";
    };
    std::string k = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n";
    k += "<name>" + xmlEscape(mission.name.empty() ? "Mission" : mission.name) + "</name>\n";
    k += "<description>" +
         xmlEscape(s.altitude == AltitudeMode::Absolute
                       ? "Altitudes are ground height from the FPV Radio Map DEM plus the planned height above ground."
                       : "Altitudes are the planned heights above ground; the viewer adds its own terrain.") +
         "</description>\n";
    k += placemark(mission.pilot);
    for (const Waypoint& w : mission.waypoints) k += placemark(w);
    k += "<Placemark><name>Route</name><LineString><altitudeMode>" + mode + "</altitudeMode><coordinates>\n";
    for (const RoutePoint& p : exportVertices(route, s.stepM)) k += kmlCoord(p.position, altitudeOf(p, world, s.altitude)) + "\n";
    k += "</coordinates></LineString></Placemark>\n</Document>\n</kml>\n";
    return k;
}

std::string toGpx(const Mission& mission, const Route& route, const IWorld& world, const ExportSettings& s) {
    // GPX has no height above ground: <ele> is always our DEM ground plus AGL.
    auto point = [&](const char* tag, const Waypoint& w) {
        return std::string("<") + tag + " " + latLonAttrs(w.position) + "><ele>" +
               num(altitudeOf(w, world, AltitudeMode::Absolute), 3) + "</ele><name>" + xmlEscape(w.label) +
               "</name><desc>" + num(w.aglM, 3) + " m above ground</desc></" + tag + ">\n";
    };
    const std::string name = xmlEscape(mission.name.empty() ? "Mission" : mission.name);
    std::string g = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<gpx version=\"1.1\" creator=\"FPV Radio Map\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n";
    g += "<metadata><name>" + name + "</name><desc>" +
         xmlEscape("Elevations are ground height from the FPV Radio Map DEM plus the planned height above ground.") +
         "</desc></metadata>\n";
    g += point("wpt", mission.pilot);
    for (const Waypoint& w : mission.waypoints) g += point("wpt", w);
    g += "<rte><name>" + name + "</name>\n" + point("rtept", mission.pilot);
    for (const Waypoint& w : mission.waypoints) g += point("rtept", w);
    if (mission.returnToPilot) g += point("rtept", mission.pilot);
    g += "</rte>\n<trk><name>" + name + "</name><trkseg>\n";
    for (const RoutePoint& p : exportVertices(route, s.stepM)) {
        g += "<trkpt " + latLonAttrs(p.position) + "><ele>" + num(altitudeOf(p, world, AltitudeMode::Absolute), 3) +
             "</ele></trkpt>\n";
    }
    g += "</trkseg></trk>\n</gpx>\n";
    return g;
}

json analyze(const json& request, const IWorld& world,
             const std::function<std::unique_ptr<ILinkEvaluator>(const json& mission)>& evalFor) {
    const json& missionJson = request.at("mission");
    const Mission mission = Mission::fromJson(missionJson);
    const json options = request.value("options", json::object());
    json reasons = json::array();
    json data = json::object();
    try {
        const Route route(mission, world);
        if (options.contains("compare")) {
            data["compare"] = compare(missionJson, options.at("compare"), world, options.value("stepM", 100.0), evalFor, reasons);
        }
        if (options.contains("export")) {
            ExportSettings s;
            s.stepM = options.value("exportStepM", 50.0);
            s.altitude = options.value("kmlAltitude", std::string("relativeToGround")) == "absolute" ? AltitudeMode::Absolute
                                                                                                     : AltitudeMode::RelativeToGround;
            json files = json::object();
            for (const json& f : options.at("export")) {
                const std::string fmt = f.get<std::string>();
                if (fmt == "kml") {
                    files["kml"] = {{"mediaType", "application/vnd.google-earth.kml+xml"}, {"content", toKml(mission, route, world, s)}};
                } else if (fmt == "gpx") {
                    files["gpx"] = {{"mediaType", "application/gpx+xml"}, {"content", toGpx(mission, route, world, s)}};
                } else {
                    throw rfmod::BadRequest("unknown export format: " + fmt);
                }
            }
            data["export"] = files;
        }
    } catch (const MissingTerrain& e) {
        reasons.push_back(reason(kId, "warning", std::string("No terrain data: ") + e.what()));
    }
    return {{"data", data}, {"reasons", reasons}};
}

}  // namespace fpv::core
