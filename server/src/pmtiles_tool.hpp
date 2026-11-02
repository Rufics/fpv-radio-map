#pragma once

// go-pmtiles (https://github.com/protomaps/go-pmtiles), the tool that cuts a region out of a
// world-wide PMTiles archive without rendering a tile again: `pmtiles extract` copies the tiles
// inside a box or polygon, reading only the directory and the tile ranges it needs.

#include "region_definition.hpp"

#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace rfmap {

/// Runs the pmtiles executable the operator configured (regions.pmtiles). It is started
/// directly, never through a shell, with a fixed argument list and an empty environment, so
/// nothing in a region definition becomes a command.
class PmtilesTool {
public:
    struct Extract {
        std::filesystem::path input;
        std::filesystem::path output;
        GeoBox bbox;
        /// A GeoJSON file of the polygon to cut along; without it the box is cut.
        std::optional<std::filesystem::path> region;
        std::optional<int> maxZoom;
    };

    explicit PmtilesTool(std::filesystem::path executable) : executable_(std::move(executable)) {}

    /// Cuts `job.output` out of `job.input`; RegionError with the tool's last output when it
    /// fails, RegionBuildCancelled when `stop` was requested (the tool is stopped too). The
    /// tool's output goes to `logFile`.
    void extract(const Extract& job, const std::filesystem::path& logFile, const std::stop_token& stop) const;
    /// The tool's version line, e.g. "pmtiles 1.31.0, commit ..."; RegionError when it cannot run.
    [[nodiscard]] std::string version(const std::filesystem::path& logFile) const;

    /// The arguments of an extract after the executable.
    [[nodiscard]] static std::vector<std::string> extractArguments(const Extract& job);

private:
    /// Runs the tool with `args` and returns its exit status; -1 when it was stopped.
    int run(const std::vector<std::string>& args, const std::filesystem::path& logFile, const std::stop_token& stop) const;
    void checkExecutable() const;
    /// The last lines of the tool's output, for an error message.
    [[nodiscard]] static std::string tail(const std::filesystem::path& logFile);

    std::filesystem::path executable_;
};

}  // namespace rfmap
