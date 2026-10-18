#include "failsafe.hpp"

#include <algorithm>
#include <cmath>

namespace fpv::failsafe {
namespace {
constexpr const char* kId = "fpv.failsafe";
}

Settings Settings::fromJson(const json& o) {
    Settings s;
    s.action = o.value("action", s.action);
    s.altitudeMode = o.value("altitudeMode", s.altitudeMode);
    s.rthAltitudeAboveHomeM = o.value("rthAltitudeAboveHomeM", s.rthAltitudeAboveHomeM);
    s.terrainClearanceM = o.value("terrainClearanceM", s.terrainClearanceM);
    s.stepM = o.value("stepM", s.stepM);
    return s;
}

std::optional<LossPoint> FailsafeAnalyzer::firstLoss(const Route& route) const {
    // Outbound only: on the way back the drone is coming home anyway.
    const double out = route.outboundLength();
    double good = 0.0;
    for (std::size_t i = 1;; ++i) {
        const double s = std::min(out, static_cast<double>(i) * s_.stepM);
        const RoutePoint p = route.at(s);
        if (link_.marginDb(p.position, p.aglM) < 0.0) {
            double lo = good, hi = s;
            while (hi - lo > 1.0) {
                const double mid = (lo + hi) / 2.0;
                const RoutePoint q = route.at(mid);
                (link_.marginDb(q.position, q.aglM) < 0.0 ? hi : lo) = mid;
            }
            return LossPoint{hi, route.at(hi)};
        }
        good = s;
        if (s >= out) return std::nullopt;
    }
}

RthPlan FailsafeAnalyzer::planRth(const Mission& mission, const Route& route, const LossPoint& loss,
                                  const battery::EnergyModel* energy) const {
    RthPlan plan{};
    const double homeGround = world_.groundMsl(mission.pilot.position);
    const double lossGround = world_.groundMsl(loss.point.position);
    const double currentMsl = lossGround + loss.point.aglM;
    const double fixedMsl = homeGround + s_.rthAltitudeAboveHomeM;
    plan.cruiseMslM = s_.altitudeMode == "fixed" ? fixedMsl
                    : s_.altitudeMode == "current" ? currentMsl
                    : std::max(currentMsl, fixedMsl);
    const InverseResult home = world_.inverse(loss.point.position, mission.pilot.position);
    plan.homeDistanceM = home.distanceM;

    // Terrain on the way home, at the DEM's own step.
    const std::vector<GroundSample> g = world_.groundProfile(loss.point.position, mission.pilot.position);
    for (const GroundSample& x : g) {
        if (x.distanceM > home.distanceM - 1e-6) break;  // landing at home is not a collision
        const double gap = plan.cruiseMslM - (x.groundMslM + x.obstacleM);
        plan.worstTerrainGapM = plan.worstTerrainGapM.has_value() ? std::min(*plan.worstTerrainGapM, gap) : gap;
        if (gap < s_.terrainClearanceM && !plan.collision) plan.collision = world_.direct(loss.point.position, home.azimuth1Deg, x.distanceM).point;
    }

    // Where control comes back on the way home.
    for (std::size_t i = 0; static_cast<double>(i) * s_.stepM <= home.distanceM; ++i) {
        const double d = static_cast<double>(i) * s_.stepM;
        const LatLon p = world_.direct(loss.point.position, home.azimuth1Deg, d).point;
        const double agl = plan.cruiseMslM - world_.groundMsl(p);
        if (agl > 0.0 && link_.marginDb(p, agl) >= 0.0) {
            plan.regainAfterM = d;
            break;
        }
    }

    if (energy) {
        plan.energyWh = energy->routeWh(route, loss.distanceAlongRouteM) +
                        energy->verticalClimbWh(plan.cruiseMslM - currentMsl) +
                        energy->levelWh(loss.point.position, mission.pilot.position);
    }
    return plan;
}

json analyze(const json& request, const IWorld& world, const ILinkProbe& link, const std::optional<json>& drone,
             const std::optional<json>& batteryItem) {
    const Mission mission = Mission::fromJson(request.at("mission"));
    const json options = request.value("options", json::object());
    const Settings settings = Settings::fromJson(options);
    json reasons = json::array();
    json data = {{"action", settings.action}, {"altitudeMode", settings.altitudeMode}};
    if (std::find(mission.links().begin(), mission.links().end(), "control") == mission.links().end()) {
        return {{"reasons", {reason(kId, "info", "Failsafe check needs the control link to be evaluated.")}}, {"data", data}};
    }
    try {
        const Route route(mission, world);
        const FailsafeAnalyzer an(world, link, settings);
        const auto loss = an.firstLoss(route);
        if (!loss) {
            data["controlLost"] = false;
            reasons.push_back(reason(kId, "info", "Failsafe: control holds along the whole way out (checked every " +
                                                      fixed(settings.stepM, 0) + " m)."));
            return {{"reasons", reasons}, {"data", data}};
        }
        data["controlLost"] = true;
        data["loss"] = {{"distanceAlongRouteM", loss->distanceAlongRouteM}, {"lat", loss->point.position.lat},
                        {"lon", loss->point.position.lon}, {"aglM", loss->point.aglM}};
        const std::string where = fixed(loss->distanceAlongRouteM / 1000.0, 2) + " km";
        if (settings.action == "land") {
            reasons.push_back(reason(kId, "blocked", "Failsafe: control is lost at " + where + " and the drone lands there."));
            return {{"reasons", reasons}, {"data", data}};
        }
        if (settings.action == "hover") {
            reasons.push_back(reason(kId, "blocked", "Failsafe: control is lost at " + where +
                                                         " and the drone hovers there until the link returns or the battery is empty."));
            return {{"reasons", reasons}, {"data", data}};
        }

        std::optional<battery::EnergyModel> energy;
        std::optional<battery::BatterySpec> spec;
        if (drone && batteryItem) {
            battery::Air air;
            const json w = request.value("weather", json());
            if (w.is_object()) {
                if (w.contains("airDensityKgM3")) air.densityKgM3 = w.at("airDensityKgM3").get<double>();
                if (w.contains("windSpeedMps")) air.windSpeedMps = w.at("windSpeedMps").get<double>();
                if (w.contains("windFromDeg")) air.windFromDeg = w.at("windFromDeg").get<double>();
                if (w.contains("temperatureC")) air.temperatureC = w.at("temperatureC").get<double>();
            }
            energy.emplace(battery::DroneSpec::fromJson(*drone), air, world);
            spec = battery::BatterySpec::fromJson(*batteryItem);
        }
        const RthPlan rth = an.planRth(mission, route, *loss, energy ? &*energy : nullptr);
        const double homeGround = world.groundMsl(mission.pilot.position);
        data["rth"] = {{"cruiseMslM", rth.cruiseMslM}, {"altitudeAboveHomeM", rth.cruiseMslM - homeGround},
                       {"homeDistanceM", rth.homeDistanceM}};
        if (rth.regainAfterM.has_value()) data["rth"]["regainAfterM"] = *rth.regainAfterM;
        if (rth.worstTerrainGapM.has_value()) data["rth"]["worstTerrainGapM"] = *rth.worstTerrainGapM;

        reasons.push_back(reason(kId, "warn",
                                 "Failsafe: control is lost at " + where + "; return-to-home flies " +
                                     fixed(rth.homeDistanceM / 1000.0, 2) + " km at " + fixed(rth.cruiseMslM - homeGround, 0) +
                                     " m above home" +
                                     (rth.regainAfterM.has_value() ? ", control back after " + fixed(*rth.regainAfterM, 0) + " m."
                                                       : ", control does not come back before home.")));
        if (rth.collision) {
            // Suggest the lowest RTH altitude that clears the terrain by the required gap.
            const double need = rth.cruiseMslM - *rth.worstTerrainGapM + settings.terrainClearanceM - homeGround;
            const double rounded = std::ceil(need);
            json fix = {{"id", "fpv.failsafe/rth-altitude"},
                        {"title", "Set RTH altitude to " + fixed(rounded, 0) + " m"},
                        {"patch", {{"modules", {{kId, {{"rthAltitudeAboveHomeM", rounded}, {"altitudeMode", "max"}}}}}}},
                        {"preview", {{{"label", "RTH altitude above home"},
                                      {"now", fixed(rth.cruiseMslM - homeGround, 0) + " m"},
                                      {"after", fixed(rounded, 0) + " m"}}}}};
            reasons.push_back(reason(kId, "blocked",
                                     "Failsafe: at this RTH altitude the way home comes within " + fixed(*rth.worstTerrainGapM, 0) +
                                         " m of terrain or clutter (negative means into it); " + fixed(settings.terrainClearanceM, 0) + " m clear is required.",
                                     fix));
            data["rth"]["suggestedAltitudeAboveHomeM"] = rounded;
        }
        if (energy && spec) {
            const json w = request.value("weather", json());
            std::optional<double> t;
            if (options.contains("temperatureC")) t = options.at("temperatureC").get<double>();
            else if (w.is_object() && w.contains("temperatureC")) t = w.at("temperatureC").get<double>();
            // Use fpv.battery's figure when it ran, so both modules quote the same usable energy.
            const json fromBattery = request.value("modules", json::object()).value("fpv.battery", json::object());
            const double usable = fromBattery.contains("usableWh")
                                      ? fromBattery.at("usableWh").get<double>()
                                      : spec->nominalWh() * spec->deratingAt(t).factor * spec->usableFraction;
            data["rth"]["energyWh"] = rth.energyWh;
            data["rth"]["usableWh"] = usable;
            if (rth.energyWh > usable) {
                reasons.push_back(reason(kId, "blocked", "Failsafe: flying home from the loss point needs " + fixed(rth.energyWh, 1) +
                                                             " Wh in total, more than the " + fixed(usable, 1) + " Wh usable."));
            }
        }
    } catch (const MissingTerrain& e) {
        reasons.push_back(reason(kId, "warn", std::string("Failsafe not checked: ") + e.what() + "."));
    } catch (const battery::Unflyable& e) {
        reasons.push_back(reason(kId, "blocked", std::string("Failsafe: return home is not flyable: ") + e.what() + "."));
    } catch (const std::domain_error& e) {
        reasons.push_back(reason(kId, "warn", std::string("Failsafe energy not checked: ") + e.what() + "."));
    }
    return {{"reasons", reasons}, {"data", data}};
}

}  // namespace fpv::failsafe
