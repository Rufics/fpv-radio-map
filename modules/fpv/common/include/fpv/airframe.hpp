#pragma once

// What kind of aircraft a drone profile describes (library drone.airframe), and the limits of a
// fixed-wing airframe that follow from its fixedWing envelope.

#include "fpv/world.hpp"

#include <rfcore/constants.hpp>

#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fpv {

/// The flight envelope of a fixed-wing airframe, as its flight controller is set.
struct FixedWingEnvelope {
    double stallSpeedMps{};
    double maxBankDeg{};
    double maxClimbAngleDeg{};
    double maxDiveAngleDeg{};
    std::optional<double> loiterRadiusM;
};

/// Thrown when the envelope of a multirotor is asked for (a programming error).
class NotFixedWing : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

class Airframe {
public:
    static constexpr double kGravity = 9.80665;  // standard, m/s²

    /// A multirotor: hovers, climbs in place and turns on the spot.
    static Airframe multirotor() { return Airframe(std::nullopt); }

    /// Reads a drone profile; without the field a drone is a multirotor. The server has validated
    /// the profile against schemas/library/drone.schema.json, which requires fixedWing for a
    /// fixed-wing airframe; one without it is a bad request.
    static Airframe fromDrone(const json& drone) {
        if (drone.value("airframe", "multirotor") != "fixed-wing") return multirotor();
        if (!drone.contains("fixedWing")) {
            throw rfmod::BadRequest("drone " + drone.value("id", std::string()) + ": a fixed-wing airframe needs fixedWing");
        }
        const json& w = drone.at("fixedWing");
        FixedWingEnvelope e{w.at("stallSpeedMps").get<double>(), w.at("maxBankDeg").get<double>(),
                            w.at("maxClimbAngleDeg").get<double>(), w.at("maxDiveAngleDeg").get<double>(), std::nullopt};
        if (w.contains("loiterRadiusM")) e.loiterRadiusM = w.at("loiterRadiusM").get<double>();
        return Airframe(e);
    }

    [[nodiscard]] bool fixedWing() const { return wing_.has_value(); }
    [[nodiscard]] std::string_view name() const { return fixedWing() ? "fixed-wing" : "multirotor"; }

    [[nodiscard]] const FixedWingEnvelope& envelope() const {
        if (!wing_.has_value()) throw NotFixedWing("a multirotor has no fixed-wing envelope");
        return *wing_;
    }

    /// Radius of a coordinated level turn at the envelope's bank: V² / (g tan φ). Over the
    /// ground, use the ground speed for V.
    [[nodiscard]] double turnRadiusM(double speedMps) const {
        return speedMps * speedMps / (kGravity * std::tan(radians(envelope().maxBankDeg)));
    }

    /// The circle the autopilot holds position on: the configured loiter radius, or the
    /// tightest turn at this speed when none is set.
    [[nodiscard]] double loiterRadiusM(double speedMps) const {
        return envelope().loiterRadiusM.value_or(turnRadiusM(speedMps));
    }

    /// Stall speed in a level turn at the envelope's bank: Vs / sqrt(cos φ), as the load factor
    /// 1 / cos φ raises it.
    [[nodiscard]] double turningStallSpeedMps() const {
        return envelope().stallSpeedMps / std::sqrt(std::cos(radians(envelope().maxBankDeg)));
    }

    [[nodiscard]] double climbGradient() const { return std::tan(radians(envelope().maxClimbAngleDeg)); }
    [[nodiscard]] double diveGradient() const { return std::tan(radians(envelope().maxDiveAngleDeg)); }

    static double radians(double deg) { return deg * rfcore::constants::kPi / 180.0; }

private:
    explicit Airframe(const std::optional<FixedWingEnvelope>& wing) : wing_(wing) {}

    std::optional<FixedWingEnvelope> wing_;
};

}  // namespace fpv
