#pragma once

#include "common.hpp"

#include <cstddef>
#include <map>
#include <utility>
#include <vector>

namespace rfmap {

/// Traces the outline of the cells set in a grid as a GeoJSON MultiPolygon. Every ring follows
/// cell edges exactly, so the line is the grid's, not an interpolation; holes become inner rings.
class PolygonTracer {
public:
    /// A grid of cols × rows cells, row 0 north, with its north-west corner at (west, north)
    /// and cells dLon wide and dLat high.
    PolygonTracer(std::size_t cols, std::size_t rows, double west, double north, double dLon, double dLat);

    /// `inside` is row-major with one flag per cell.
    [[nodiscard]] json multiPolygon(const std::vector<bool>& inside) const;

private:
    /// A cell edge on the integer grid: x = column edge (0..cols), y = row edge (0..rows, down).
    struct Edge {
        long x0 = 0;
        long y0 = 0;
        long x1 = 0;
        long y1 = 0;
        bool used = false;
    };
    using Ring = std::vector<std::pair<long, long>>;
    using EdgesFrom = std::multimap<std::pair<long, long>, std::size_t>;

    /// Each boundary edge of an inside cell, directed with the inside on its left.
    [[nodiscard]] std::vector<Edge> boundaryEdges(const std::vector<bool>& inside) const;
    /// The unused edge continuing `e`, or edges.size().
    [[nodiscard]] static std::size_t nextEdge(const std::vector<Edge>& edges, const EdgesFrom& from, const Edge& e);
    /// Chains edges from `start` until the ring closes, marking them used.
    [[nodiscard]] static Ring traceRing(std::vector<Edge>& edges, const EdgesFrom& from, std::size_t start);
    /// Drops vertices in the middle of straight runs.
    [[nodiscard]] static Ring simplified(const Ring& ring);
    /// Signed area in map orientation: outer rings are positive.
    [[nodiscard]] static double signedArea(const Ring& r);
    [[nodiscard]] static bool ringContains(const Ring& r, double x, double y);
    /// The index in `outers` of the smallest outer ring holding the hole, or outers.size().
    [[nodiscard]] static std::size_t outerOfHole(const std::vector<Ring>& rings, const std::vector<std::size_t>& outers,
                                                 const Ring& hole);
    [[nodiscard]] json coordinates(const Ring& r) const;

    std::size_t cols_;
    std::size_t rows_;
    double west_;
    double north_;
    double dLon_;
    double dLat_;
};

}  // namespace rfmap
