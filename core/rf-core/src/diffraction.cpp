#include "rfcore/diffraction.hpp"

#include "rfcore/errors.hpp"
#include "rfcore/propagation.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace rfcore {

BullingtonDiffraction::BullingtonDiffraction(Options options) : options_(options) {}
BullingtonDiffraction::BullingtonDiffraction() : options_() {}

Decibel BullingtonDiffraction::empiricalCorrection(Decibel knifeEdgeLoss, Metres pathLength) {
    const double luc = knifeEdgeLoss.value();
    return Decibel((1.0 - std::exp(-luc / 6.0)) * (10.0 + 0.02 * pathLength.value() / 1000.0));
}

DiffractionResult BullingtonDiffraction::compute(const PathGeometry& geometry, Metres lambda) const {
    const std::vector<PlanePoint>& tops = geometry.tops();
    if (tops.size() < 3) {
        throw std::invalid_argument("BullingtonDiffraction: the profile has no intermediate points");
    }
    const PlanePoint t = geometry.antennaStart();
    const PlanePoint r = geometry.antennaEnd();

    DiffractionResult out{};
    out.minFresnelClearance = std::numeric_limits<double>::infinity();

    // Steepest elevation above the ray seen from each antenna, and the highest ν.
    double bestFromStart = -std::numeric_limits<double>::infinity();
    double bestFromEnd = -std::numeric_limits<double>::infinity();
    std::size_t idxStart = 0;
    std::size_t idxEnd = 0;
    double bestNu = -std::numeric_limits<double>::infinity();
    std::size_t idxNu = 0;

    for (std::size_t i = 1; i + 1 < tops.size(); ++i) {
        const PlanePoint& p = tops[i];
        const RayRelation rel = geometry.relate(p);
        if (!(rel.d1.value() > 0.0) || !(rel.d2.value() > 0.0)) {
            continue;  // Behind an antenna in the plane; cannot obstruct the ray.
        }
        // Angles above the ray, as tangents, seen from each end.
        const double tanFromStart = rel.heightAbove.value() / rel.d1.value();
        const double tanFromEnd = rel.heightAbove.value() / rel.d2.value();
        if (tanFromStart > bestFromStart) { bestFromStart = tanFromStart; idxStart = i; }
        if (tanFromEnd > bestFromEnd) { bestFromEnd = tanFromEnd; idxEnd = i; }

        if (const double v = KnifeEdge::nu(rel.heightAbove, lambda, rel.d1, rel.d2); v > bestNu) {
            bestNu = v;
            idxNu = i;
        }

        const Metres r1 = FresnelZone::radius(1, lambda, rel.d1, rel.d2);
        const double clearance = -rel.heightAbove.value() / r1.value();
        if (clearance < out.minFresnelClearance) {
            out.minFresnelClearance = clearance;
            out.minClearanceSample = i;
            out.firstFresnelRadiusAtMin = r1;
        }
    }

    if (bestFromStart <= 0.0) {
        // Line of sight: the governing edge is the point with the largest ν (ITU-R P.526 §4.5, LOS case).
        const RayRelation rel = geometry.relate(tops[idxNu]);
        out.lineOfSight = true;
        out.nu = bestNu;
        out.edgeHeight = rel.heightAbove;
        out.d1 = rel.d1;
        out.d2 = rel.d2;
        out.dominantSample = idxNu;
    } else {
        // Trans-horizon / obstructed: intersect the steepest line from each antenna.
        const PlanePoint ps = tops[idxStart];
        const PlanePoint pe = tops[idxEnd];
        // Direction from the first antenna.
        const double ax = ps.x - t.x;
        const double ay = ps.y - t.y;
        // Direction from the second antenna.
        const double bx = pe.x - r.x;
        const double by = pe.y - r.y;
        const double denom = ax * by - ay * bx;
        if (denom == 0.0) {
            throw DegenerateGeometryError("BullingtonDiffraction: tangent lines are parallel");
        }
        const double u = ((r.x - t.x) * by - (r.y - t.y) * bx) / denom;
        const PlanePoint edge{t.x + u * ax, t.y + u * ay};
        const RayRelation rel = geometry.relate(edge);
        out.lineOfSight = false;
        out.nu = KnifeEdge::nu(rel.heightAbove, lambda, rel.d1, rel.d2);
        out.edgeHeight = rel.heightAbove;
        out.d1 = rel.d1;
        out.d2 = rel.d2;
        out.dominantSample = idxStart;
    }

    out.loss = KnifeEdge::loss(out.nu);
    return out;
}

}  // namespace rfcore
