#include "rfcore/fresnel_integrals.hpp"

#include "rfcore/constants.hpp"

#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>

namespace rfcore {
namespace {

constexpr double kEps = std::numeric_limits<double>::epsilon();
constexpr double kTiny = std::numeric_limits<double>::min();
constexpr double kSeriesLimit = 1.5;
constexpr int kMaxIterations = 1000;

// Power series (Abramowitz & Stegun 7.3.11/7.3.13) for 0 <= x <= 1.5.
void series(double x, double& c, double& s) {
    const double halfPi = constants::kPi / 2.0;
    const double fact = halfPi * x * x;
    double sumC = x;
    double sumS = 0.0;
    double term = x;
    double sign = 1.0;
    bool odd = true;
    double sum = 0.0;
    int n = 3;
    for (int k = 1; k <= kMaxIterations; ++k) {
        term *= fact / k;
        sum += sign * term / n;
        const double test = std::abs(sum) * kEps;
        if (odd) {
            sign = -sign;
            sumS = sum;
            sum = sumC;
        } else {
            sumC = sum;
            sum = sumS;
        }
        if (term < test) {
            c = sumC;
            s = sumS;
            return;
        }
        odd = !odd;
        n += 2;
    }
    throw std::runtime_error("FresnelIntegrals: series did not converge");
}

// Continued fraction for x > 1.5. Returns w = (1/2 - C) + i (1/2 - S).
std::complex<double> complementContinuedFraction(double x) {
    using cd = std::complex<double>;
    const double pix2 = constants::kPi * x * x;
    cd b(1.0, -pix2);
    cd cc(1.0 / kTiny, 0.0);
    cd d = 1.0 / b;
    cd h = d;
    double n = -1.0;
    for (int k = 2; k <= kMaxIterations; ++k) {
        n += 2.0;
        const double a = -n * (n + 1.0);
        b += 4.0;
        d = 1.0 / (a * d + b);
        cc = b + a / cc;
        const cd del = cc * d;
        h *= del;
        if (std::abs(del.real() - 1.0) + std::abs(del.imag()) < kEps) {
            h *= cd(x, -x);
            // The phase πx²/2 is huge for large x. Reduce x² modulo 4 (one full turn) before
            // multiplying by π/2, using the exact product x² = hi + lo, so no digits are lost.
            const double hi = x * x;
            const double lo = std::fma(x, x, -hi);
            const double turns = std::fmod(hi, 4.0) + lo;
            const double angle = constants::kPi / 2.0 * turns;
            const cd phase(std::cos(angle), std::sin(angle));
            // C + iS = (1+i)/2 * (1 - e^{iπx²/2} h), so the complement is (1+i)/2 * e^{iπx²/2} h.
            return cd(0.5, 0.5) * phase * h;
        }
    }
    throw std::runtime_error("FresnelIntegrals: continued fraction did not converge");
}

}  // namespace

FresnelIntegrals::Values FresnelIntegrals::evaluate(double x) {
    if (!std::isfinite(x)) {
        throw std::invalid_argument("FresnelIntegrals: argument must be finite");
    }
    const double ax = std::abs(x);
    double c = 0.0;
    double s = 0.0;
    double halfMinusC = 0.5;
    double halfMinusS = 0.5;

    if (ax < std::sqrt(kTiny)) {
        c = ax;
        s = 0.0;
        halfMinusC = 0.5 - c;
        halfMinusS = 0.5;
    } else if (ax <= kSeriesLimit) {
        series(ax, c, s);
        halfMinusC = 0.5 - c;
        halfMinusS = 0.5 - s;
    } else {
        const std::complex<double> w = complementContinuedFraction(ax);
        halfMinusC = w.real();
        halfMinusS = w.imag();
        c = 0.5 - halfMinusC;
        s = 0.5 - halfMinusS;
    }

    if (x < 0.0) {
        // C and S are odd functions. The complements stay those of |x|.
        c = -c;
        s = -s;
    }
    return {c, s, halfMinusC, halfMinusS};
}

}  // namespace rfcore
