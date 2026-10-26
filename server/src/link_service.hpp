#pragma once

#include "builtins.hpp"
#include "common.hpp"
#include "library.hpp"
#include "raster.hpp"
#include "weather.hpp"

#include <rfcore/link_budget.hpp>
#include <rfcore/terrain_profile.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rfmap {

class ModuleRunner;

enum class LinkKind { Control, Video };
std::string toString(LinkKind k);
LinkKind linkKindFromString(std::string_view s);  ///< HttpError 422 otherwise

/// One end of a path. Heights are entered above ground; MSL comes from the DEM.
struct Terminal {
    rfcore::GeoPoint position;
    double aglM;
    static Terminal fromJson(const json& point);  ///< mission.schema.json#/$defs/point
};

/// A mission linkSetup with its library items looked up and checked.
struct ResolvedLink {
    LinkKind kind{};
    json radio;
    json groundAntenna;
    json airAntenna;
    double txPowerDbm{};
    double requiredMarginDb{};
    std::vector<LibraryRef> refs;
};

/// Everything a calculation depends on besides the two ends: time, the mission's module
/// choices and the forecast resolved for it.
struct CalcContext {
    TimePoint time;
    json missionModules = json::object();  ///< mission.modules
    std::optional<ForecastAt> forecast;    ///< set when rf.weather is on and opted in
    [[nodiscard]] bool optedIn(const std::string& id) const {
        return missionModules.is_object() && missionModules.contains(id);
    }
    [[nodiscard]] json optionsFor(const std::string& id) const {
        return optedIn(id) && missionModules.at(id).is_object() ? missionModules.at(id) : json::object();
    }
    /// Which forecast tier fed the calculation: hourly | long-range | none (beyond the forecast,
    /// k = 4/3) | off (rf.weather not opted in by the mission, or disabled by the admin).
    [[nodiscard]] std::string forecastTier() const { return forecast ? forecast->point.value("tier", "none") : "off"; }
};

/// A terrain profile between the ground and air ends, clutter applied, ready for any link.
struct PreparedPath {
    Terminal ground;
    Terminal air;
    double distanceM = 0.0;
    std::optional<rfcore::TerrainProfile> profile;
    std::vector<rfcore::MissingSpan> missing;
    std::vector<rfcore::UncertaintyNote> notes;
    [[nodiscard]] bool complete() const { return profile.has_value(); }
};

/// A failure of an optional module during a calculation. Never fails the request.
struct ModuleFailure {
    std::string moduleId;
    std::string error;
    [[nodiscard]] json toJson() const { return {{"module", moduleId}, {"error", error}}; }
    /// Adds this failure to a result's list unless the module is listed already.
    void appendTo(json& list) const;
    /// Adds each failure once, keeping the entries already in `list`.
    static void appendAll(json& list, const std::vector<ModuleFailure>& failures);
};

struct LinkEvaluation {
    json result;  ///< OpenAPI LinkResult
    double marginDb{};
    std::vector<ModuleFailure> failures;
};

/// The radio calculation for one path: rf-core link budget, rf-ext refraction, rain, clutter
/// and uncertainty, and the loss contributions of opted-in modules.
class LinkService {
public:
    LinkService(std::shared_ptr<const DemSource> dem, std::shared_ptr<const LandCoverSource> landcover,
                const Library& library, const BuiltinModules& builtins, ModuleRunner* modules, std::string version);

    [[nodiscard]] ResolvedLink resolve(const json& linkSetup, LinkKind kind) const;

    /// The DEM profile from `from` to `to`, missing spans reported, never filled. HttpError 503
    /// without a DEM, 422 when both ends are the same point or the path would need more than
    /// kMaxProfileSamples DEM samples.
    [[nodiscard]] rfcore::ProfileResult terrain(const rfcore::GeoPoint& from, const rfcore::GeoPoint& to) const;
    /// Builds the profile with terrain() and applies clutter.
    [[nodiscard]] PreparedPath prepare(const Terminal& ground, const Terminal& air, const CalcContext& ctx) const;
    /// 3000 km at 30 m: far beyond any link the model is for, and a bound on the profile's size.
    static constexpr double kMaxProfileSamples = 100'000.0;

    /// `withBand`: run the uncertainty analysis (9 evaluations); `withProfile`: put the profile
    /// in the result. The path must be complete.
    [[nodiscard]] LinkEvaluation evaluate(const PreparedPath& path, const ResolvedLink& link, const CalcContext& ctx,
                                          bool withBand, bool withProfile) const;

    /// Same as evaluate, for raw radio parameters (host call link.evaluate without a mission).
    struct RawEnds {
        double frequencyHz{};
        double txPowerDbm{};
        Terminal tx;
        Terminal rx;
        double txGainDbi{};
        double txFeederDb{};
        double rxGainDbi{};
        double rxFeederDb{};
        double sensitivityDbm{};
        double requiredMarginDb{};
    };
    [[nodiscard]] LinkEvaluation evaluateRaw(const RawEnds& ends, const CalcContext& ctx) const;

    [[nodiscard]] const DemSource* dem() const { return dem_.get(); }
    [[nodiscard]] const LandCoverSource* landcover() const { return landcover_.get(); }
    [[nodiscard]] const Library& library() const { return library_; }
    [[nodiscard]] const std::string& version() const { return version_; }

    [[nodiscard]] static json profileJson(const rfcore::TerrainProfile& p, double stepM);
    [[nodiscard]] static json missingJson(const std::vector<rfcore::MissingSpan>& spans);
    /// OpenAPI MissingData for the path start → end: the spans along it, and `missingData`
    /// {bbox [w, s, e, n], source} covering the DEM samples that had no value.
    [[nodiscard]] json missingData(const std::vector<rfcore::MissingSpan>& spans, const rfcore::GeoPoint& start,
                                   const rfcore::GeoPoint& end) const;
    /// The smallest [w, s, e, n] holding both boxes; either may be null.
    [[nodiscard]] static json unionBbox(const json& a, const json& b);
    /// Throws HttpError 503 when the server has no DEM loaded.
    const DemSource& requireDem() const;

private:
    struct Budget;
    /// rf.clutter on the profile: the admin's class heights minus the mission's disabled classes.
    void applyClutter(rfcore::TerrainProfile& profile, std::vector<rfcore::UncertaintyNote>& notes,
                      const CalcContext& ctx) const;
    [[nodiscard]] LinkEvaluation run(const PreparedPath& path, const Budget& b, const CalcContext& ctx, bool withBand,
                                     bool withProfile) const;

    std::shared_ptr<const DemSource> dem_;
    std::shared_ptr<const LandCoverSource> landcover_;
    const Library& library_;
    const BuiltinModules& builtins_;
    ModuleRunner* modules_;
    std::string version_;
};

}  // namespace rfmap
