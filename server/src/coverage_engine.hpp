#pragma once

#include "builtins.hpp"
#include "link_service.hpp"
#include "module_runner.hpp"
#include "thread_pool.hpp"

#include <atomic>
#include <cstddef>
#include <stdexcept>

namespace rfmap {

/// Where a coverage calculation reports its progress and learns that it should stop.
class CoverageProgress {
public:
    virtual ~CoverageProgress() = default;
    /// Called with 0..1 as rows finish, from the calculation's worker threads.
    virtual void report(double fraction) = 0;
    /// Polled between rows; true stops the calculation.
    [[nodiscard]] virtual bool cancelled() const = 0;
};

/// Thrown when a coverage calculation stops because its CoverageProgress asked it to.
class CalculationCancelled : public std::runtime_error {
public:
    CalculationCancelled() : std::runtime_error("cancelled") {}
};

/// A lat/lon-regular grid around the pilot: cellM is exact at the pilot's latitude (meridian
/// and parallel arc per degree there); further north or south a cell's east-west size changes
/// with the parallel, and cellLatDeg/cellLonDeg say exactly what was used.
class CoverageGrid {
public:
    CoverageGrid(const rfcore::GeoPoint& pilot, double radiusM, double cellM, double aglM);

    [[nodiscard]] rfcore::GeoPoint centre(std::size_t row, std::size_t col) const;
    [[nodiscard]] std::size_t cols() const { return cols_; }
    [[nodiscard]] std::size_t rows() const { return rows_; }
    [[nodiscard]] std::size_t cells() const { return cols_ * rows_; }
    [[nodiscard]] double radiusM() const { return radius_; }
    [[nodiscard]] double aglM() const { return agl_; }
    [[nodiscard]] double cellM() const { return cell_; }
    [[nodiscard]] double dLat() const { return dLat_; }
    [[nodiscard]] double dLon() const { return dLon_; }
    [[nodiscard]] double north() const { return north_; }
    [[nodiscard]] double west() const { return west_; }
    [[nodiscard]] double south() const { return north_ - static_cast<double>(rows_) * dLat_; }
    [[nodiscard]] double east() const { return west_ + static_cast<double>(cols_) * dLon_; }

    /// Cells a coverage request would compute, for the size limit; 0 without the needed fields.
    [[nodiscard]] static std::size_t cellsFor(const json& mission);

private:
    double radius_;
    double cell_;
    double agl_;
    double dLat_ = 0.0;
    double dLon_ = 0.0;
    double north_ = 0.0;
    double west_ = 0.0;
    std::size_t cols_ = 0;
    std::size_t rows_ = 0;
};

/// The coverage calculation of one link: every grid cell within the radius evaluated on the
/// pool's threads, the reachable cells outlined, then the coverage.analysis modules. Runs
/// inside the caller's InvocationScope, which the worker threads join.
class CoverageEngine {
public:
    CoverageEngine(LinkService& links, const BuiltinModules& builtins, const ModuleRunner& modules);

    struct Request {
        const json& mission;
        LinkKind kind;
        const ResolvedLink& link;
        const CalcContext& ctx;
        const json& libraryRefs;
        std::string hash;
    };
    /// OpenAPI CoverageGrid. Throws CalculationCancelled when `progress` says so.
    [[nodiscard]] json run(const Request& request, CoverageProgress& progress, ThreadPool& pool) const;

private:
    struct Run;
    /// One pool thread of a coverage calculation: takes rows until none are left.
    void worker(Run& run) const;
    void row(Run& run, std::size_t r) const;
    [[nodiscard]] json gridJson(const Request& request, const CoverageGrid& g, const Run& run) const;
    void runModules(const Request& request, std::size_t cells, json& grid) const;
    /// A module's coverage.analyze answer in the grid: its reasons, and the layers that match the
    /// grid cell for cell.
    static void addModule(json& grid, const json& resp, const std::string& moduleId, std::size_t cells);

    LinkService& links_;
    const BuiltinModules& builtins_;
    const ModuleRunner& modules_;
};

}  // namespace rfmap
