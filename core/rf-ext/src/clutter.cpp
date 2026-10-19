#include "rfext/clutter.hpp"

#include <algorithm>
#include <format>

namespace rfext {

const std::vector<LandCover>& allLandCovers() {
    using enum LandCover;
    static const std::vector all = {Tree,    Shrubland, Grassland, Cropland,  BuiltUp,   BareSparse,
                                    SnowIce, Water,     Wetland,   Mangroves, MossLichen};
    return all;
}

std::string landCoverName(LandCover c) {
    using enum LandCover;
    switch (c) {
        case Tree: return "Tree cover";
        case Shrubland: return "Shrubland";
        case Grassland: return "Grassland";
        case Cropland: return "Cropland";
        case BuiltUp: return "Built-up";
        case BareSparse: return "Bare / sparse vegetation";
        case SnowIce: return "Snow and ice";
        case Water: return "Permanent water";
        case Wetland: return "Herbaceous wetland";
        case Mangroves: return "Mangroves";
        case MossLichen: return "Moss and lichen";
    }
    return "Unknown";
}

std::optional<LandCover> landCoverFromCode(int code) {
    for (LandCover c : allLandCovers()) {
        if (static_cast<int>(c) == code) return c;
    }
    return std::nullopt;
}

ClutterConditioner::ClutterConditioner(std::map<LandCover, ClassSetting> settings) : settings_(std::move(settings)) {}

namespace {

void addSource(std::vector<std::string>& sources, const std::string& s) {
    if (!s.empty() && std::ranges::find(sources, s) == sources.end()) sources.push_back(s);
}

/// Obstacle height of one sample in an enabled class: the raster's where it has one, else the
/// admin setting. Records where the height came from.
std::optional<rfcore::Metres> classHeight(const ClassSetting& set, LandCover cls, const rfcore::ProfileSample& s,
                                          std::vector<std::string>& sources) {
    std::optional<rfcore::Metres> h;
    if (set.raster) {
        h = set.raster->height(s.position);
        if (h.has_value()) addSource(sources, set.raster->name());
    }
    if (!h.has_value() && set.height.has_value()) {
        h = set.height;
        addSource(sources, landCoverName(cls) + ": " + (set.heightSource.empty() ? "admin setting" : set.heightSource));
    }
    return h;
}

}  // namespace

ClutterConditioner::Report ClutterConditioner::apply(rfcore::TerrainProfile& profile) const {
    Report r;
    auto& samples = profile.mutableSamples();
    for (std::size_t i = 1; i + 1 < samples.size(); ++i) {
        rfcore::ProfileSample& s = samples[i];
        if (!s.landCover.has_value()) {
            ++r.samplesWithoutCover;
            continue;
        }
        const auto cls = landCoverFromCode(*s.landCover);
        if (!cls) {
            ++r.samplesWithoutCover;
            continue;
        }
        const auto it = settings_.find(*cls);
        if (it == settings_.end() || !it->second.enabled) continue;
        const std::optional<rfcore::Metres> h = classHeight(it->second, *cls, s, r.sources);
        if (!h) {
            ++r.samplesWithoutHeight;
            continue;
        }
        if (h->value() > 0.0) {
            s.obstacleHeight = std::max(s.obstacleHeight, *h);
            ++r.samplesRaised;
            ++r.perClass[*cls];
        }
    }
    return r;
}

std::vector<rfcore::UncertaintyNote> ClutterConditioner::Report::notes() const {
    std::vector<rfcore::UncertaintyNote> out;
    if (samplesRaised > 0) {
        std::string src;
        for (const auto& s : sources) src += (src.empty() ? "" : "; ") + s;
        out.emplace_back("Clutter", std::format("{} samples raised by trees or buildings, treated as opaque (an "
                                                "upper bound on loss through foliage). Heights: {}",
                                                samplesRaised, src));
    }
    if (samplesWithoutHeight > 0) {
        out.emplace_back("Clutter", std::format("{} samples in an enabled class have no height data and were left bare",
                                                samplesWithoutHeight));
    }
    if (samplesWithoutCover > 0) {
        out.emplace_back("Clutter", std::format("{} samples have no land cover data", samplesWithoutCover));
    }
    return out;
}

}  // namespace rfext
