#include "grid_outline.hpp"

#include <cmath>
#include <limits>
#include <ranges>

namespace rfmap {

PolygonTracer::PolygonTracer(std::size_t cols, std::size_t rows, double west, double north, double dLon, double dLat)
    : cols_(cols), rows_(rows), west_(west), north_(north), dLon_(dLon), dLat_(dLat) {}

std::vector<PolygonTracer::Edge> PolygonTracer::boundaryEdges(const std::vector<bool>& inside) const {
    const auto in = [&inside, this](long r, long c) {
        return r >= 0 && c >= 0 && r < static_cast<long>(rows_) && c < static_cast<long>(cols_) &&
               inside[static_cast<std::size_t>(r) * cols_ + static_cast<std::size_t>(c)];
    };
    std::vector<Edge> edges;
    for (long r = 0; r < static_cast<long>(rows_); ++r) {
        for (long c = 0; c < static_cast<long>(cols_); ++c) {
            if (!in(r, c)) continue;
            // y grows southwards here, so counter-clockwise on the map runs: bottom left to
            // bottom right, up the east side, top right to top left, down the west side.
            if (!in(r + 1, c)) edges.emplace_back(c, r + 1, c + 1, r + 1);
            if (!in(r, c + 1)) edges.emplace_back(c + 1, r + 1, c + 1, r);
            if (!in(r - 1, c)) edges.emplace_back(c + 1, r, c, r);
            if (!in(r, c - 1)) edges.emplace_back(c, r, c, r + 1);
        }
    }
    return edges;
}

std::size_t PolygonTracer::nextEdge(const std::vector<Edge>& edges, const EdgesFrom& from, const Edge& e) {
    // At a vertex two inside cells touch only by a corner: turn left (in map terms), which
    // keeps them as separate rings.
    const long dx = e.x1 - e.x0;
    const long dy = e.y1 - e.y0;
    std::size_t next = edges.size();
    int bestRank = 9;
    const auto [first, last] = from.equal_range({e.x1, e.y1});
    for (const std::size_t candidate : std::views::values(std::ranges::subrange(first, last))) {
        const Edge& n = edges[candidate];
        if (n.used) continue;
        const long ndx = n.x1 - n.x0;
        const long ndy = n.y1 - n.y0;
        // y points down, so a left turn on the map is (dy, -dx) here.
        int rank = 2;
        if (ndx == dy && ndy == -dx) rank = 0;
        else if (ndx == dx && ndy == dy) rank = 1;
        if (rank < bestRank) {
            bestRank = rank;
            next = candidate;
        }
    }
    return next;
}

PolygonTracer::Ring PolygonTracer::traceRing(std::vector<Edge>& edges, const EdgesFrom& from, std::size_t start) {
    Ring ring;
    // The ring closes back at `start`; running out of edges cannot happen on a closed boundary.
    for (std::size_t cur = start; cur != edges.size();) {
        Edge& e = edges[cur];
        e.used = true;
        ring.emplace_back(e.x0, e.y0);
        const bool closed = e.x1 == edges[start].x0 && e.y1 == edges[start].y0;
        cur = closed ? edges.size() : nextEdge(edges, from, e);
    }
    return ring;
}

PolygonTracer::Ring PolygonTracer::simplified(const Ring& ring) {
    Ring simple;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const auto& [px, py] = ring[(i + ring.size() - 1) % ring.size()];
        const auto& [qx, qy] = ring[i];
        const auto& [sx, sy] = ring[(i + 1) % ring.size()];
        if ((qx - px) * (sy - qy) != (qy - py) * (sx - qx)) simple.push_back(ring[i]);
    }
    return simple;
}

double PolygonTracer::signedArea(const Ring& r) {
    // x east, y north = -y here.
    double a = 0.0;
    for (std::size_t i = 0; i < r.size(); ++i) {
        const auto& [px, py] = r[i];
        const auto& [qx, qy] = r[(i + 1) % r.size()];
        a += static_cast<double>(px) * static_cast<double>(-qy) - static_cast<double>(qx) * static_cast<double>(-py);
    }
    return a / 2.0;
}

bool PolygonTracer::ringContains(const Ring& r, double x, double y) {
    bool c = false;
    std::size_t j = r.size() - 1;
    for (std::size_t i = 0; i < r.size(); j = i++) {
        const auto [ix, iy] = r[i];
        const auto [jx, jy] = r[j];
        const auto xi = static_cast<double>(ix);
        const auto yi = static_cast<double>(iy);
        const auto xj = static_cast<double>(jx);
        const auto yj = static_cast<double>(jy);
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) c = !c;
    }
    return c;
}

std::size_t PolygonTracer::outerOfHole(const std::vector<Ring>& rings, const std::vector<std::size_t>& outers,
                                       const Ring& hole) {
    // A point just inside the hole: the middle of its first edge, nudged to the hole side
    // (the hole is on the right of a hole edge, i.e. away from the inside cells).
    const auto [px, py] = hole[0];
    const auto [qx, qy] = hole[1];
    const double mx = (static_cast<double>(px) + static_cast<double>(qx)) / 2.0;
    const double my = (static_cast<double>(py) + static_cast<double>(qy)) / 2.0;
    const auto dx = static_cast<double>(qx - px);
    const auto dy = static_cast<double>(qy - py);
    const double len = std::hypot(dx, dy);
    const double tx = mx - dy / len * 0.25;
    const double ty = my + dx / len * 0.25;
    std::size_t best = outers.size();
    double bestArea = std::numeric_limits<double>::infinity();
    for (std::size_t o = 0; o < outers.size(); ++o) {
        if (const double a = signedArea(rings[outers[o]]); a < bestArea && ringContains(rings[outers[o]], tx, ty)) {
            bestArea = a;
            best = o;
        }
    }
    return best;
}

json PolygonTracer::coordinates(const Ring& r) const {
    json ring = json::array();
    for (const auto& [x, y] : r) {
        ring.push_back({west_ + static_cast<double>(x) * dLon_, north_ - static_cast<double>(y) * dLat_});
    }
    if (!ring.empty()) ring.push_back(ring.front());
    return ring;
}

json PolygonTracer::multiPolygon(const std::vector<bool>& inside) const {
    // Boundary edges of the inside cells, chained into rings.
    std::vector<Edge> edges = boundaryEdges(inside);
    EdgesFrom from;
    for (std::size_t i = 0; i < edges.size(); ++i) from.insert({{edges[i].x0, edges[i].y0}, i});
    std::vector<Ring> rings;
    for (std::size_t start = 0; start < edges.size(); ++start) {
        if (!edges[start].used) rings.push_back(simplified(traceRing(edges, from, start)));
    }

    std::vector<std::size_t> outers;
    std::vector<std::vector<std::size_t>> holesOf(rings.size());
    for (std::size_t i = 0; i < rings.size(); ++i) {
        if (signedArea(rings[i]) > 0.0) outers.push_back(i);
    }
    for (std::size_t i = 0; i < rings.size(); ++i) {
        if (signedArea(rings[i]) > 0.0 || rings[i].size() < 2) continue;
        if (const std::size_t best = outerOfHole(rings, outers, rings[i]); best < outers.size()) {
            holesOf[outers[best]].push_back(i);
        }
    }
    json polys = json::array();
    for (std::size_t o : outers) {
        json poly = json::array({coordinates(rings[o])});
        for (std::size_t h : holesOf[o]) poly.push_back(coordinates(rings[h]));
        polys.push_back(poly);
    }
    return {{"type", "MultiPolygon"}, {"coordinates", polys}};
}

}  // namespace rfmap
