#pragma once

#include <fpv/route.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fpv::battery {

struct DroneSpec {
    std::string id;
    int version = 0;
    double massKg{};
    double cruiseAirspeedMps{};
    double cruisePowerW{};          ///< measured, sea-level ISA density
    double powerDensityExponent{};  ///< n in P = P0 (ρ0/ρ)^n; 0 = density not applied
    std::string climbMethod;        ///< potential-energy | measured
    double climbEfficiency = 0.0;
    double climbPowerW = 0.0;
    double climbRateMps = 0.0;

    static DroneSpec fromJson(const json& j);
};

struct BatterySpec {
    std::string id;
    int version = 0;
    int cellsSeries{};
    double capacityAh{};
    double nominalCellVoltage{};
    double usableFraction{};
    std::vector<std::pair<double, double>> derating;  ///< (°C, factor), sorted by temperature

    static BatterySpec fromJson(const json& j);

    [[nodiscard]] double nominalWh() const { return cellsSeries * nominalCellVoltage * capacityAh; }

    struct Derated {
        double factor;
        std::string note;  ///< how the factor was obtained
    };
    /// Factor at a temperature, linear between measured points. Outside the measured range it is
    /// not extrapolated: throws std::domain_error.
    [[nodiscard]] Derated deratingAt(std::optional<double> temperatureC) const;
};

/// The air the flight happens in. Unknown wind means the result is calm-air only, and says so.
struct Air {
    std::optional<double> densityKgM3;
    std::optional<double> windSpeedMps;
    std::optional<double> windFromDeg;  ///< meteorological: where the wind comes from
    std::optional<double> temperatureC;

    /// From a weather.point answer, with windSpeedMps, windFromDeg and temperatureC in the
    /// options taking precedence.
    static Air fromJson(const json& weather, const json& options);
};

/// Thrown when the drone cannot hold its course in this wind.
class Unflyable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Electrical energy for flying a path at cruise airspeed in a steady wind, plus climbing.
class EnergyModel {
public:
    static constexpr double kSeaLevelDensity = 1.225;  // ISA, kg/m³
    static constexpr double kGravity = 9.80665;        // standard, m/s²

    EnergyModel(DroneSpec drone, const Air& air, const IWorld& world);

    /// P0 (ρ0/ρ)^n. Throws std::domain_error if n ≠ 0 and density is unknown.
    [[nodiscard]] double cruisePowerW() const;

    /// Ground speed on a course: Vg = W·ĉ + sqrt(Va² − (W×ĉ)²). Throws Unflyable when the
    /// crosswind exceeds the airspeed or the drone would be pushed backwards.
    [[nodiscard]] double groundSpeedMps(double courseDeg) const;

    /// Energy for the geodesic from `a` to `b`, the height changing linearly from `heightA` to
    /// `heightB` in the reference of `altitude`.
    [[nodiscard]] double segmentWh(const LatLon& a, double heightA, const LatLon& b, double heightB,
                                   const AltitudeModel& altitude) const;

    /// Cruise energy for the geodesic a→b at constant altitude (no climbing).
    [[nodiscard]] double levelWh(const LatLon& a, const LatLon& b) const;

    /// Energy to climb straight up by `riseM` before flying on: m g Δh / η, or for a measured
    /// climb the climb power for Δh / climb rate (no forward flight during it).
    [[nodiscard]] double verticalClimbWh(double riseM) const;

    /// Energy along the route from its start to distance s.
    [[nodiscard]] double routeWh(const Route& route, double s) const;

    [[nodiscard]] const DroneSpec& drone() const { return drone_; }

private:
    [[nodiscard]] double cruiseWh(const LatLon& a, double azimuthDeg, double length) const;
    /// Climbing energy for a segment; descending gives nothing back.
    [[nodiscard]] double climbWh(const LatLon& a, double heightA, const LatLon& b, double heightB,
                                 const AltitudeModel& altitude) const;
    /// Climbing energy for a total rise.
    [[nodiscard]] double riseWh(double riseM) const;

    DroneSpec drone_;
    Air air_;
    const IWorld& world_;
    double power_{};  ///< cruise power, set once from the drone and the air
};

struct TurnBack {
    double distanceAlongRouteM;
    double returnM;
    LatLon position;
    double aglM;
    double heightM;  ///< in the mission's altitude reference
    std::size_t leg;
};

struct Plan {
    double energyWh;
    double usableWh;
    double neededWh;
    double rangeM;
    double neededM;
    double shortfallM;
    std::optional<TurnBack> turnBack;  ///< only when the return is part of the mission and energy runs short
    std::vector<std::string> notes;
};

/// Solves for range and the turn-back point.
class BatteryPlanner {
public:
    /// `scanStepM`: spacing of the search for the last point you can still return from; the
    /// bracketed point is then refined by bisection to 1 mm.
    BatteryPlanner(const EnergyModel& model, const BatterySpec& battery, double scanStepM = 50.0)
        : model_(model), battery_(battery), step_(scanStepM) {}

    [[nodiscard]] Plan plan(const Mission& mission, const Route& route, const IWorld& world,
                            std::optional<double> temperatureC) const;

private:
    const EnergyModel& model_;
    const BatterySpec& battery_;
    double step_;
};

}  // namespace fpv::battery
