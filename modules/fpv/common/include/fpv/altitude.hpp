#pragma once

#include "fpv/world.hpp"

#include <string>
#include <string_view>

namespace fpv {

/// What a waypoint's height is measured from (mission.altitudeRef).
enum class AltitudeReference {
    Launch,  ///< above the ground at the pilot: a barometric altitude from take-off (`altM`)
    Agl,     ///< above the ground under the drone (`aglM`)
};

/// How waypoint heights become drone altitudes. Between two waypoints the height changes linearly
/// with distance along the leg in the mission's reference: with Launch the drone flies a straight
/// line in altitude and the terrain does not move it; with Agl it follows the terrain. The pilot's
/// own height is its antenna above the ground in both references, which is the same number because
/// the launch ground is the ground at the pilot.
class AltitudeModel {
public:
    AltitudeModel(AltitudeReference reference, double launchGroundMslM)
        : reference_(reference), launchGround_(launchGroundMslM) {}

    /// Reads "launch" or "agl"; a mission without the field is an AGL mission (the original format).
    static AltitudeReference parseReference(const json& mission) {
        const std::string ref = mission.value("altitudeRef", "agl");
        return ref == "launch" ? AltitudeReference::Launch : AltitudeReference::Agl;
    }

    /// The mission field that holds waypoint heights in this reference.
    static std::string_view waypointField(AltitudeReference reference) {
        return reference == AltitudeReference::Launch ? "altM" : "aglM";
    }

    [[nodiscard]] AltitudeReference reference() const { return reference_; }
    [[nodiscard]] bool followsTerrain() const { return reference_ == AltitudeReference::Agl; }
    [[nodiscard]] double launchGroundMslM() const { return launchGround_; }
    [[nodiscard]] std::string_view waypointField() const { return waypointField(reference_); }

    /// Altitude above mean sea level of a drone at `heightM` in this reference over `groundMslM`.
    [[nodiscard]] double mslM(double heightM, double groundMslM) const {
        return followsTerrain() ? groundMslM + heightM : launchGround_ + heightM;
    }

    /// Height above the ground under the drone; negative when the drone would be below it.
    [[nodiscard]] double aglM(double heightM, double groundMslM) const { return mslM(heightM, groundMslM) - groundMslM; }

    /// The height in this reference that puts the drone at `mslM` over `groundMslM`.
    [[nodiscard]] double heightFor(double mslM, double groundMslM) const {
        return followsTerrain() ? mslM - groundMslM : mslM - launchGround_;
    }

private:
    AltitudeReference reference_;
    double launchGround_;
};

}  // namespace fpv
