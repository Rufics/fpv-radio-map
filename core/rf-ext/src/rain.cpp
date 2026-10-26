#include "rfext/rain.hpp"

#include <rfcore/constants.hpp>
#include <rfcore/path_geometry.hpp>

#include <array>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace rfext {
namespace {

struct Term {
    double a;
    double b;
    double c;
};

// ITU-R P.838-3 Tables 1-4.
constexpr std::array<Term, 4> kKH{{{-5.33980, -0.10008, 1.13098},
                                   {-0.35351, 1.26970, 0.45400},
                                   {-0.23789, 0.86036, 0.15354},
                                   {-0.94158, 0.64552, 0.16817}}};
constexpr double kKHm = -0.18961;
constexpr double kKHc = 0.71147;
constexpr std::array<Term, 4> kKV{{{-3.80595, 0.56934, 0.81061},
                                   {-3.44965, -0.22911, 0.51059},
                                   {-0.39902, 0.73042, 0.11899},
                                   {0.50167, 1.07319, 0.27195}}};
constexpr double kKVm = -0.16398;
constexpr double kKVc = 0.63297;
constexpr std::array<Term, 5> kAH{{{-0.14318, 1.82442, -0.55187},
                                   {0.29591, 0.77564, 0.19822},
                                   {0.32177, 0.63773, 0.13164},
                                   {-5.37610, -0.96230, 1.47828},
                                   {16.1721, -3.29980, 3.43990}}};
constexpr double kAHm = 0.67849;
constexpr double kAHc = -1.95537;
constexpr std::array<Term, 5> kAV{{{-0.07771, 2.33840, -0.76284},
                                   {0.56727, 0.95545, 0.54039},
                                   {-0.20238, 1.14520, 0.26809},
                                   {-48.2991, 0.791669, 0.116226},
                                   {48.5833, 0.791459, 0.116479}}};
constexpr double kAVm = -0.053739;
constexpr double kAVc = 0.83433;

template <std::size_t N>
double series(const std::array<Term, N>& t, double m, double c, double x) {
    double sum = 0.0;
    for (const Term& term : t) {
        const double z = (x - term.b) / term.c;
        sum += term.a * std::exp(-z * z);
    }
    return sum + m * x + c;
}

}  // namespace

RainAttenuation::Coefficients RainAttenuation::coefficients(rfcore::Hertz frequency) {
    const double fGHz = frequency.value() / 1e9;
    if (!(fGHz >= 1.0 && fGHz <= 1000.0)) throw std::invalid_argument("P.838-3 is defined for 1-1000 GHz");
    const double x = std::log10(fGHz);
    return {std::pow(10.0, series(kKH, kKHm, kKHc, x)), series(kAH, kAHm, kAHc, x),
            std::pow(10.0, series(kKV, kKVm, kKVc, x)), series(kAV, kAVm, kAVc, x)};
}

double RainAttenuation::specificDbPerKm(rfcore::Hertz frequency, double rate, double elevationDeg, double tiltDeg) {
    if (!(rate >= 0.0)) throw std::invalid_argument("rain rate must be >= 0");
    if (rate == 0.0) return 0.0;
    const Coefficients c = coefficients(frequency);
    const double deg = rfcore::constants::kPi / 180.0;
    const double cos2 = std::cos(elevationDeg * deg) * std::cos(elevationDeg * deg) * std::cos(2.0 * tiltDeg * deg);
    const double k = (c.kH + c.kV + (c.kH - c.kV) * cos2) / 2.0;
    const double alpha = (c.kH * c.alphaH + c.kV * c.alphaV + (c.kH * c.alphaH - c.kV * c.alphaV) * cos2) / (2.0 * k);
    return k * std::pow(rate, alpha);
}

RainContributor::RainContributor(double rate, double tiltDeg, std::string rateSource)
    : rate_(rate), tilt_(tiltDeg), source_(std::move(rateSource)) {}

rfcore::LossContribution RainContributor::contribute(const rfcore::PathContext& ctx) const {
    if (rate_ <= 0.0) return {};
    const auto a = ctx.geometry.antennaStart();
    const auto b = ctx.geometry.antennaEnd();
    // Elevation of the ray in the path plane; in the plane the ray is straight.
    const double elevationDeg = std::atan2(std::abs(b.y - a.y), std::abs(b.x - a.x)) * 180.0 / rfcore::constants::kPi;
    const double gamma = RainAttenuation::specificDbPerKm(ctx.frequency, rate_, elevationDeg, tilt_);
    const double loss = gamma * ctx.geometry.rayLength().value() / 1000.0;
    std::ostringstream label;
    label.precision(3);
    label << "Rain " << rate_ << " mm/h";
    return {{{"rf.weather/rain", label.str(), rfcore::Decibel(loss), "ITU-R P.838-3, rate from " + source_}},
            {{"Rain", "hourly mean rate from " + source_ +
                          ", taken as uniform along the whole path; real showers are patchier and vary within the hour"}}};
}

}  // namespace rfext
