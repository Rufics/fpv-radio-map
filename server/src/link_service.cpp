#include "link_service.hpp"

#include "module_runner.hpp"
#include "service_errors.hpp"

#include <rfcore/constants.hpp>
#include <rfcore/earth_model.hpp>
#include <rfcore/geodesy.hpp>
#include <rfcore/propagation.hpp>
#include <rfcore/receiver.hpp>
#include <rfext/clutter.hpp>
#include <rfext/rain.hpp>
#include <rfext/refractivity.hpp>
#include <rfext/uncertainty.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <set>
#include <sstream>

namespace rfmap {

using rfcore::Decibel;
using rfcore::DecibelIsotropic;
using rfcore::DecibelMilliwatt;
using rfcore::Hertz;
using rfcore::Metres;

void ModuleFailure::appendTo(json& list) const {
    const bool seen = std::ranges::any_of(list, [this](const json& j) { return j.value("module", "") == moduleId; });
    if (!seen) list.push_back(toJson());
}

void ModuleFailure::appendAll(json& list, const std::vector<ModuleFailure>& failures) {
    for (const auto& f : failures) f.appendTo(list);
}

std::string toString(LinkKind k) { return k == LinkKind::Control ? "control" : "video"; }

LinkKind linkKindFromString(std::string_view s) {
    if (s == "control") return LinkKind::Control;
    if (s == "video") return LinkKind::Video;
    throw HttpError(422, "link must be control or video");
}

Terminal Terminal::fromJson(const json& p) {
    return {{p.at("lat").get<double>(), p.at("lon").get<double>()}, p.at("aglM").get<double>()};
}

namespace {

std::string fmt(double v, int precision = 6) {
    std::ostringstream s;
    s.precision(precision);
    s << v;
    return s.str();
}

/// P.838 polarisation tilt for an antenna's polarisation, if it has a defined one.
std::optional<double> tiltOf(std::string_view polarization) {
    if (polarization == "rhcp" || polarization == "lhcp") return 45.0;
    if (polarization == "linear-horizontal") return 0.0;
    if (polarization == "linear-vertical") return 90.0;
    return std::nullopt;
}

std::string sourceText(const json& item) {
    if (!item.contains("source")) return "no source given";
    const json& s = item.at("source");
    return s.value("kind", "") + ": " + s.value("reference", "");
}

/// Reverses a profile so the transmitter is at its start. The path azimuth at the mid point
/// turns by 180°, which leaves the radius of curvature unchanged.
rfcore::TerrainProfile reversed(const rfcore::TerrainProfile& p) {
    std::vector<rfcore::ProfileSample> s(p.samples().rbegin(), p.samples().rend());
    const Metres length = p.length();
    for (auto& x : s) x.distance = length - x.distance;
    s.front().distance = Metres(0.0);
    const double az = std::fmod(p.azimuthDeg() + 180.0, 360.0);
    return rfcore::TerrainProfile(std::move(s), p.sourceName(), az, p.midLatitudeDeg());
}

json pointJson(const rfcore::ProfileSample& s, double distanceM) {
    return {{"distanceM", distanceM},
            {"groundMslM", s.groundMsl.value()},
            {"obstacleM", s.obstacleHeight.value()},
            {"lat", s.position.latitudeDeg},
            {"lon", s.position.longitudeDeg}};
}

/// Land-cover codes the mission turned off in its rf.clutter options.
std::set<int> disabledClasses(const json& opts) {
    std::set<int> off;
    if (!opts.contains("disabledClasses") || !opts.at("disabledClasses").is_array()) return off;
    for (const json& c : opts.at("disabledClasses")) {
        if (c.is_number_integer()) off.insert(c.get<int>());
    }
    return off;
}

/// The admin's clutter class settings, with the mission's disabled classes switched off.
std::map<rfext::LandCover, rfext::ClassSetting> clutterSettings(const json& admin, const std::set<int>& off) {
    std::map<rfext::LandCover, rfext::ClassSetting> settings;
    for (const auto& [code, s] : admin.items()) {
        const auto cls = rfext::landCoverFromCode(std::stoi(code));
        if (!cls) continue;
        rfext::ClassSetting set;
        set.enabled = s.value("enabled", false) && !off.contains(std::stoi(code));
        if (s.contains("heightM")) set.height = Metres(s.at("heightM").get<double>());
        set.heightSource = s.value("source", "");
        settings[*cls] = set;
    }
    return settings;
}

/// The effective-earth factor a path uses and where it came from.
struct EarthFactor {
    double k = rfcore::constants::kStandardEffectiveEarthFactor;
    std::string source = "standard atmosphere (ITU-R P.453, -40 N/km)";
};

/// k from the forecast gradient over the layer the path lives in, or the standard atmosphere
/// with a note saying why.
EarthFactor earthFactor(const rfcore::TerrainProfile& profile, const CalcContext& ctx, bool weatherOn,
                        double topMsl, std::vector<rfcore::UncertaintyNote>& notes) {
    EarthFactor out;
    if (!weatherOn) {
        notes.emplace_back("Refraction", "weather not used for this calculation; standard atmosphere k = 4/3");
        return out;
    }
    if (!ctx.forecast.has_value()) return out;
    for (const auto& n : ctx.forecast->notes) notes.push_back(n);
    if (!ctx.forecast->covered()) return out;
    const auto est = rfext::refractionForPath(ctx.forecast->column, Metres(topMsl));
    if (!est.has_value()) {
        notes.emplace_back("Refraction", "the forecast column does not reach the higher antenna (" + fmt(topMsl) +
                                             " m MSL); standard k = 4/3 used");
        return out;
    }
    const Metres radius = rfcore::EarthModel::radiusOfCurvature(profile.midLatitudeDeg(), profile.azimuthDeg());
    if (const double kf = rfcore::EarthModel::factorFromRefractivityGradient(est->gradientPerKm, radius);
        kf > 0.0 && std::isfinite(kf)) {
        out.k = kf;
        out.source = est->description + ", forecast";
    } else {
        notes.emplace_back("Refraction", "the forecast gradient " + fmt(est->gradientPerKm) +
                                             " N/km means ducting (k <= 0); the effective-earth model does not "
                                             "apply, so the standard atmosphere is shown instead");
    }
    return out;
}

/// Adds the P.838 rain term when the forecast has rain and the link can take it.
void addRain(rfcore::LinkEvaluator& evaluator, const CalcContext& ctx, Hertz frequency,
             const std::optional<double>& tiltDeg, std::vector<rfcore::UncertaintyNote>& notes) {
    if (!ctx.forecast.has_value() || !ctx.forecast->covered() || !ctx.forecast->rainMmPerHour.has_value() ||
        !(*ctx.forecast->rainMmPerHour > 0.0)) {
        return;
    }
    if (frequency.value() < 1e9) {
        notes.emplace_back("Rain", "ITU-R P.838-3 covers 1-1000 GHz; rain loss at " + fmt(frequency.value() / 1e6) +
                                       " MHz is not computed (forecast " + fmt(*ctx.forecast->rainMmPerHour) + " mm/h)");
    } else if (tiltDeg.has_value()) {
        evaluator.addContributor(std::make_shared<rfext::RainContributor>(*ctx.forecast->rainMmPerHour, *tiltDeg,
                                                                          ctx.forecast->sourceLabel));
    }
}

/// The note on the DEM's stated vertical error, if there is one to make.
void addTerrainNote(json& uncertainty, const DemSource& dem) {
    const std::optional<double> statedError = dem.statedErrorM();
    if (!statedError.has_value()) {
        uncertainty.push_back({{"what", "Terrain data"},
                               {"detail", "the DEM's stated vertical error is not configured on this server "
                                          "(data.demStatedErrorM), so no terrain error is carried"}});
    } else if (!dem.statedErrorSource().empty()) {
        uncertainty.push_back({{"what", "Terrain data"},
                               {"detail", "stated error " + fmt(*statedError) + " m from " + dem.statedErrorSource()}});
    }
}

/// The diffraction result with its points in ground -> air order whatever the direction of
/// transmission.
json diffractionJson(const rfcore::DiffractionResult& d, const std::vector<rfcore::ProfileSample>& groundToAir,
                     bool txIsGround) {
    const std::size_t n = groundToAir.size();
    const auto groundIndex = [txIsGround, n](std::size_t i) { return txIsGround ? i : n - 1 - i; };
    json diff = {{"lineOfSight", d.lineOfSight},
                 {"nu", d.nu},
                 {"edgeHeightM", d.edgeHeight.value()},
                 {"d1M", d.d1.value()},
                 {"d2M", d.d2.value()},
                 {"lossDb", d.loss.value()},
                 {"minFresnelClearance", d.minFresnelClearance},
                 {"firstFresnelRadiusM", d.firstFresnelRadiusAtMin.value()}};
    const auto& s = groundToAir[groundIndex(d.dominantSample)];
    diff["dominant"] = pointJson(s, s.distance.value());
    const auto& m = groundToAir[groundIndex(d.minClearanceSample)];
    diff["minClearancePoint"] = pointJson(m, m.distance.value());
    return diff;
}

/// The best analog picture grade the SNR reaches, or "lost".
std::string analogGrade(const json& grades, double snr) {
    std::vector<std::pair<double, std::string>> sorted;
    for (const json& g : grades) {
        sorted.emplace_back(g.at("requiredSnrDb").get<double>(), g.at("label").get<std::string>());
    }
    std::ranges::sort(sorted, std::greater{});
    for (const auto& [need, label] : sorted) {
        if (snr >= need) return label;
    }
    return "lost";
}

/// Latency and, for analog video, the SNR and picture grade from the radio profile.
void addRadioDetails(json& res, const json& radio, const std::optional<DecibelMilliwatt>& noiseFloor,
                     double receivedDbm) {
    if (!radio.is_object()) return;
    if (radio.contains("latencyMs")) {
        res["latencyMs"] = radio.at("latencyMs").at("typical");
        if (radio.at("latencyMs").contains("max")) res["latencyMaxMs"] = radio.at("latencyMs").at("max");
    }
    if (radio.value("kind", "") != "video-analog") return;
    if (noiseFloor.has_value()) {
        const double snr = receivedDbm - noiseFloor->value();
        res["snrDb"] = snr;
        res["analogGrade"] = analogGrade(radio.at("analogGrades"), snr);
    } else {
        res["uncertainty"].push_back({{"what", "Analog grade"},
                                      {"detail", "the radio profile gives a datasheet sensitivity, not the noise "
                                                 "figure and bandwidth the picture grades need"}});
    }
}

json refsJson(const std::vector<LibraryRef>& refs) {
    json out = json::array();
    for (const auto& ref : refs) out.push_back(ref.toJson());
    return out;
}

/// A module that failed in any of the band's evaluations is listed once.
std::vector<ModuleFailure> oncePerModule(const std::vector<ModuleFailure>& failures) {
    std::vector<ModuleFailure> unique;
    for (const auto& f : failures) {
        if (std::ranges::none_of(unique, [&f](const ModuleFailure& u) { return u.moduleId == f.moduleId; })) {
            unique.push_back(f);
        }
    }
    return unique;
}

}  // namespace

/// The radio side of one evaluation: who transmits, powers, gains, sensitivity.
struct LinkService::Budget {
    std::optional<LinkKind> kind;
    Hertz frequency;
    bool txIsGround = true;
    rfcore::TransmitterEnd tx{};
    rfcore::ReceiverEnd rx{};
    std::optional<double> tiltDeg;
    std::optional<DecibelMilliwatt> noiseFloor;  ///< including NF and extra noise, for analog grades
    json radio;                                  ///< null for raw evaluations
    std::vector<LibraryRef> refs;
    std::vector<rfcore::UncertaintyNote> notes;
};

LinkService::LinkService(std::shared_ptr<const DemSource> dem, std::shared_ptr<const LandCoverSource> landcover,
                         const Library& library, const BuiltinModules& builtins, ModuleRunner* modules,
                         std::string version)
    : dem_(std::move(dem)),
      landcover_(std::move(landcover)),
      library_(library),
      builtins_(builtins),
      modules_(modules),
      version_(std::move(version)) {}

const DemSource& LinkService::requireDem() const {
    if (!dem_) throw HttpError(503, "this server has no terrain model loaded (data.dem is not configured)");
    return *dem_;
}

ResolvedLink LinkService::resolve(const json& setup, LinkKind kind) const {
    if (!builtins_.enabled(BuiltinModules::kRadioProfiles)) {
        throw HttpError(503, "radio profiles (rf.radio-profiles) are disabled by the admin");
    }
    ResolvedLink r;
    r.kind = kind;
    auto [radio, radioRef] = library_.require("radio-profiles", setup.at("radio").get<std::string>());
    auto [ground, groundRef] = library_.require("antennas", setup.at("groundAntenna").get<std::string>());
    auto [air, airRef] = library_.require("antennas", setup.at("airAntenna").get<std::string>());
    const std::string rk = radio.at("kind").get<std::string>();
    if (const bool kindOk =
            kind == LinkKind::Control ? rk == "control" : (rk == "video-digital" || rk == "video-analog");
        !kindOk) {
        throw HttpError(422, "radio profile '" + radioRef.id + "' is a " + rk + " profile, not usable for the " +
                                 toString(kind) + " link");
    }
    r.txPowerDbm = setup.at("txPowerDbm").get<double>();
    if (const auto& steps = radio.at("txPowerDbm");
        std::ranges::none_of(steps, [&r](const json& s) { return s.get<double>() == r.txPowerDbm; })) {
        throw HttpError(422, "radio profile '" + radioRef.id + "' has no " + fmt(r.txPowerDbm) + " dBm power step",
                        {{"steps", steps}});
    }
    r.requiredMarginDb = setup.at("requiredMarginDb").get<double>();
    r.radio = std::move(radio);
    r.groundAntenna = std::move(ground);
    r.airAntenna = std::move(air);
    r.refs = {radioRef, groundRef, airRef};
    return r;
}

PreparedPath LinkService::prepare(const Terminal& ground, const Terminal& air, const CalcContext& ctx) const {
    const DemSource& dem = requireDem();
    PreparedPath p{ground, air, 0.0, std::nullopt, {}, {}};
    const rfcore::Geodesy geo;
    p.distanceM = geo.inverse(ground.position, air.position).distance.value();
    if (!(p.distanceM > 0.0)) throw HttpError(422, "the two ends of the path are the same point");
    // Diffraction needs at least one point between the ends; on a path shorter than two DEM
    // steps the middle sample still takes the DEM's own value, nothing is interpolated.
    std::optional<Metres> step;
    if (p.distanceM < 2.0 * dem.nativeResolution().value()) step = Metres(p.distanceM / 2.0);
    rfcore::ProfileResult built = rfcore::ProfileBuilder(dem).build(ground.position, air.position, step);
    p.missing = std::move(built.missing);
    if (!built.profile.has_value()) return p;
    rfcore::TerrainProfile& profile = p.profile.emplace(std::move(*built.profile));

    if (landcover_) {
        for (auto& s : profile.mutableSamples()) s.landCover = landcover_->code(s.position);
    }
    if (builtins_.enabled(BuiltinModules::kClutter) && ctx.optedIn(BuiltinModules::kClutter)) {
        applyClutter(profile, p.notes, ctx);
    }
    return p;
}

void LinkService::applyClutter(rfcore::TerrainProfile& profile, std::vector<rfcore::UncertaintyNote>& notes,
                               const CalcContext& ctx) const {
    if (!landcover_) {
        notes.emplace_back("Clutter", "no land cover layer is configured on this server; clutter not applied");
        return;
    }
    const std::set<int> off = disabledClasses(ctx.optionsFor(BuiltinModules::kClutter));
    const json admin = builtins_.options(BuiltinModules::kClutter).value("classes", json::object());
    const auto report = rfext::ClutterConditioner(clutterSettings(admin, off)).apply(profile);
    for (auto& n : report.notes()) notes.push_back(std::move(n));
    if (!off.empty()) {
        std::string list;
        for (const int c : off) list += std::format("{}{}", list.empty() ? "" : ", ", c);
        notes.emplace_back("Clutter", std::format("classes turned off for this mission: {}", list));
    }
}

LinkEvaluation LinkService::evaluate(const PreparedPath& path, const ResolvedLink& link, const CalcContext& ctx,
                                     bool withBand, bool withProfile) const {
    Budget b;
    b.kind = link.kind;
    b.radio = link.radio;
    b.refs = link.refs;
    b.frequency = Hertz(link.radio.at("frequencyHz").get<double>());
    b.txIsGround = link.kind == LinkKind::Control;
    const json& txAnt = b.txIsGround ? link.groundAntenna : link.airAntenna;
    const json& rxAnt = b.txIsGround ? link.airAntenna : link.groundAntenna;
    const Terminal& txEnd = b.txIsGround ? path.ground : path.air;
    const Terminal& rxEnd = b.txIsGround ? path.air : path.ground;
    b.tx.antennaAgl = Metres(txEnd.aglM);
    b.tx.antennaGain = DecibelIsotropic(txAnt.at("gainDbi").get<double>());
    b.tx.feederLoss = Decibel(txAnt.value("feederLossDb", 0.0));
    b.tx.power = DecibelMilliwatt(link.txPowerDbm);
    b.rx.antennaAgl = Metres(rxEnd.aglM);
    b.rx.antennaGain = DecibelIsotropic(rxAnt.at("gainDbi").get<double>());
    b.rx.feederLoss = Decibel(rxAnt.value("feederLossDb", 0.0));
    b.rx.requiredMargin = Decibel(link.requiredMarginDb);

    const json& sens = link.radio.at("sensitivity");
    const double extra = link.radio.value("extraNoiseDb", 0.0);
    if (sens.at("method") == "computed") {
        const Hertz bw(sens.at("bandwidthHz").get<double>());
        const Decibel nf(sens.at("noiseFigureDb").get<double>() + extra);
        b.rx.sensitivity = rfcore::ReceiverSensitivity::compute(bw, nf, Decibel(sens.at("requiredSnrDb").get<double>()));
        b.noiseFloor = rfcore::ReceiverSensitivity::thermalNoiseFloor(bw) + nf;
        b.notes.emplace_back("Receiver sensitivity", "computed 10log10(kT0B)+30+NF+SNR = " +
                                                         fmt(b.rx.sensitivity.value()) + " dBm from " +
                                                         sourceText(sens));
    } else {
        b.rx.sensitivity = DecibelMilliwatt(sens.at("dBm").get<double>());
        b.notes.emplace_back("Receiver sensitivity", "datasheet figure " + fmt(b.rx.sensitivity.value()) +
                                                         " dBm as stated by " + sourceText(sens) +
                                                         "; test conditions are the vendor's");
    }
    for (const json* ant : {&link.groundAntenna, &link.airAntenna}) {
        const auto& band = ant->at("bandHz");
        const double lo = band.at(0).get<double>();
        const double hi = band.at(1).get<double>();
        if (b.frequency.value() < lo || b.frequency.value() > hi) {
            b.notes.emplace_back("Antenna", ant->value("name", "antenna") + " is specified for " + fmt(lo / 1e6) +
                                                "-" + fmt(hi / 1e6) + " MHz, not " + fmt(b.frequency.value() / 1e6) +
                                                " MHz; its gain figure may not hold");
        }
    }
    const auto t1 = tiltOf(link.groundAntenna.value("polarization", ""));
    if (const auto t2 = tiltOf(link.airAntenna.value("polarization", ""));
        t1.has_value() && t2.has_value() && *t1 == *t2) {
        b.tiltDeg = t1;
    } else {
        b.notes.emplace_back("Polarisation", "ground and air antennas differ in polarisation; the mismatch loss is "
                                             "not modelled and rain loss is not applied");
    }
    for (const LibraryRef& r : link.refs) {
        if (!r.verified) {
            b.notes.emplace_back("Library",
                                 std::format("{}/{} v{} is not verified by the admin yet", r.kind, r.id, r.version));
        }
    }
    return run(path, b, ctx, withBand, withProfile);
}

LinkEvaluation LinkService::evaluateRaw(const RawEnds& e, const CalcContext& ctx) const {
    Budget b;
    b.frequency = Hertz(e.frequencyHz);
    b.txIsGround = true;  // the profile runs from tx to rx
    b.tx.antennaAgl = Metres(e.tx.aglM);
    b.tx.antennaGain = DecibelIsotropic(e.txGainDbi);
    b.tx.feederLoss = Decibel(e.txFeederDb);
    b.tx.power = DecibelMilliwatt(e.txPowerDbm);
    b.rx.antennaAgl = Metres(e.rx.aglM);
    b.rx.antennaGain = DecibelIsotropic(e.rxGainDbi);
    b.rx.feederLoss = Decibel(e.rxFeederDb);
    b.rx.sensitivity = DecibelMilliwatt(e.sensitivityDbm);
    b.rx.requiredMargin = Decibel(e.requiredMarginDb);
    b.notes.emplace_back("Polarisation", "not given in this request; rain loss is not applied");
    const PreparedPath path = prepare(e.tx, e.rx, ctx);
    if (!path.complete()) {
        throw HttpError(422, "missing elevation data on part of the path; nothing is guessed",
                        missingData(path.missing, e.tx.position, e.rx.position));
    }
    return run(path, b, ctx, true, false);
}

LinkEvaluation LinkService::run(const PreparedPath& path, const Budget& b, const CalcContext& ctx, bool withBand,
                                bool withProfile) const {
    if (!path.profile) throw ServiceInvariantError("evaluate needs a complete profile");
    LinkEvaluation out;
    std::vector<rfcore::UncertaintyNote> notes = path.notes;
    notes.insert(notes.end(), b.notes.begin(), b.notes.end());

    // The profile runs ground -> air; rf-core puts the transmitter at its start.
    const rfcore::TerrainProfile profile = b.txIsGround ? *path.profile : reversed(*path.profile);
    const auto& samples = profile.samples();
    const double txMsl = samples.front().groundMsl.value() + b.tx.antennaAgl.value();
    const double rxMsl = samples.back().groundMsl.value() + b.rx.antennaAgl.value();

    // Effective earth: k from the forecast gradient over the layer the path lives in.
    const bool weatherOn = builtins_.enabled(BuiltinModules::kWeather) && ctx.optedIn(BuiltinModules::kWeather);
    const EarthFactor earth = earthFactor(profile, ctx, weatherOn, std::max(txMsl, rxMsl), notes);

    rfcore::BullingtonDiffraction::Options dopt;
    dopt.useEmpiricalCorrection = ctx.optionsFor("rf.core").value("useEmpiricalCorrection", false);
    rfcore::LinkEvaluator evaluator{rfcore::EarthModel(earth.k), rfcore::BullingtonDiffraction(dopt)};

    if (weatherOn) addRain(evaluator, ctx, b.frequency, b.tiltDeg, notes);
    if (modules_) {
        const Terminal& txT = b.txIsGround ? path.ground : path.air;
        const Terminal& rxT = b.txIsGround ? path.air : path.ground;
        for (auto& c : modules_->lossContributors(ctx, {txT, rxT}, landcover_.get(), out.failures)) {
            evaluator.addContributor(std::move(c));
        }
    }

    const rfcore::LinkResult r = evaluator.evaluate(profile, b.frequency, b.tx, b.rx);
    out.marginDb = r.margin.value();

    json terms = json::array();
    for (const auto& t : r.terms) {
        terms.push_back({{"id", t.id}, {"label", t.label}, {"valueDb", t.value.value()}, {"source", t.source}});
    }
    json uncertainty = json::array();
    for (const auto& n : r.uncertainty) uncertainty.push_back({{"what", n.what}, {"detail", n.detail}});
    for (const auto& n : notes) uncertainty.push_back({{"what", n.what}, {"detail", n.detail}});
    addTerrainNote(uncertainty, requireDem());

    const auto& gs = path.profile->samples();
    const json diff = diffractionJson(r.diffraction, gs, b.txIsGround);

    json& res = out.result;
    res = {{"frequencyHz", b.frequency.value()},
           {"distanceM", path.distanceM},
           {"rayLengthM", r.rayLength.value()},
           {"terms", terms},
           {"txPowerDbm", b.tx.power.value()},
           {"receivedDbm", r.received.value()},
           {"sensitivityDbm", b.rx.sensitivity.value()},
           {"requiredMarginDb", b.rx.requiredMargin.value()},
           {"requiredDbm", r.required.value()},
           {"marginDb", r.margin.value()},
           {"reachable", r.reachable()},
           {"diffraction", diff},
           {"uncertainty", uncertainty},
           {"earth", {{"effectiveEarthFactor", earth.k}, {"source", earth.source}}},
           {"model", "rfmap-server " + version_ + " / rf-core: free space ITU-R P.525, Bullington ITU-R P.526-15 with "
                                                  "exact J(nu), exact effective-earth geometry" +
                         std::string(dopt.useEmpiricalCorrection ? ", P.526 empirical correction on" : "")}};
    auto endJson = [](const Terminal& t, const rfcore::ProfileSample& s) {
        return json{{"lat", t.position.latitudeDeg},
                    {"lon", t.position.longitudeDeg},
                    {"groundMslM", s.groundMsl.value()},
                    {"aglM", t.aglM},
                    {"antennaMslM", s.groundMsl.value() + t.aglM}};
    };
    res["ends"] = {{"ground", endJson(path.ground, gs.front())},
                   {"air", endJson(path.air, gs.back())},
                   {"transmitter", b.txIsGround ? "ground" : "air"}};
    if (b.kind) res["kind"] = toString(*b.kind);
    if (!b.refs.empty()) res["library"] = refsJson(b.refs);
    addRadioDetails(res, b.radio, b.noiseFloor, r.received.value());

    if (withBand) {
        if (builtins_.enabled(BuiltinModules::kUncertainty)) {
            const rfext::MarginBand band = rfext::UncertaintyAnalyzer(evaluator).analyze(profile, b.frequency, b.tx, b.rx);
            res["band"] = {{"nominalDb", band.nominal.value()},
                           {"worstDb", band.worst.value()},
                           {"bestDb", band.best.value()},
                           {"verdict", rfext::toString(band.verdict)},
                           {"source", "rf.uncertainty: DEM stated error propagated through every term"}};
        } else {
            res["uncertainty"].push_back({{"what", "Uncertainty band"}, {"detail", "rf.uncertainty is disabled by the admin"}});
        }
    }
    if (withProfile) res["profile"] = profileJson(*path.profile, (gs[1].distance - gs[0].distance).value());

    out.failures = oncePerModule(out.failures);
    json failures = json::array();
    for (const auto& f : out.failures) failures.push_back(f.toJson());
    res["moduleFailures"] = failures;
    return out;
}

json LinkService::profileJson(const rfcore::TerrainProfile& p, double stepM) {
    json pts = json::array();
    for (const auto& s : p.samples()) {
        json j = pointJson(s, s.distance.value());
        j["uncertaintyM"] = s.uncertainty.value();
        if (s.landCover.has_value()) j["landCover"] = *s.landCover;
        pts.push_back(std::move(j));
    }
    return {{"source", p.sourceName()}, {"lengthM", p.length().value()}, {"stepM", stepM}, {"points", pts}};
}

json LinkService::missingData(const std::vector<rfcore::MissingSpan>& spans, const rfcore::GeoPoint& start,
                               const rfcore::GeoPoint& end) const {
    const DemSource& dem = requireDem();
    const rfcore::Geodesy geo;
    const double az = geo.inverse(start, end).azimuthStartDeg;
    json bbox = nullptr;
    for (const auto& s : spans) {
        const rfcore::GeoPoint a = geo.direct(start, az, s.from);
        const rfcore::GeoPoint b = geo.direct(start, az, s.to);
        // The box covers the samples on the geodesic, which is where the DEM was read.
        const std::vector<rfcore::GeoPoint> pts =
            s.to.value() > s.from.value() ? geo.sample(a, b, dem.nativeResolution()) : std::vector<rfcore::GeoPoint>{a};
        for (const auto& p : pts) {
            bbox = unionBbox(bbox, json::array({p.longitudeDeg, p.latitudeDeg, p.longitudeDeg, p.latitudeDeg}));
        }
    }
    return {{"status", "missing-data"},
            {"missing", missingJson(spans)},
            {"missingData", {{"bbox", bbox}, {"source", dem.name()}}}};
}

json LinkService::unionBbox(const json& a, const json& b) {
    if (a.is_null()) return b;
    if (b.is_null()) return a;
    return json::array({std::min(a[0].get<double>(), b[0].get<double>()), std::min(a[1].get<double>(), b[1].get<double>()),
                        std::max(a[2].get<double>(), b[2].get<double>()), std::max(a[3].get<double>(), b[3].get<double>())});
}

json LinkService::missingJson(const std::vector<rfcore::MissingSpan>& spans) {
    json out = json::array();
    for (const auto& s : spans) out.push_back({{"fromM", s.from.value()}, {"toM", s.to.value()}});
    return out;
}

}  // namespace rfmap
