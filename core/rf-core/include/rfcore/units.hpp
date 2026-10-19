#pragma once

// Strong unit types. A value carries its unit in the type, so adding metres to decibels,
// or passing hertz where metres are expected, does not compile. All values are SI doubles;
// rounding happens only when a value is shown to a person.

#include <cmath>
#include <compare>

namespace rfcore {

template <class Tag>
class Quantity {
public:
    constexpr Quantity() = default;
    constexpr explicit Quantity(double value) : value_(value) {}

    [[nodiscard]] constexpr double value() const { return value_; }

    friend constexpr Quantity operator+(Quantity lhs, Quantity rhs) { return Quantity(lhs.value_ + rhs.value_); }
    friend constexpr Quantity operator-(Quantity lhs, Quantity rhs) { return Quantity(lhs.value_ - rhs.value_); }
    constexpr Quantity operator-() const { return Quantity(-value_); }
    friend constexpr Quantity operator*(Quantity q, double factor) { return Quantity(q.value_ * factor); }
    friend constexpr Quantity operator*(double factor, Quantity q) { return Quantity(factor * q.value_); }
    friend constexpr Quantity operator/(Quantity q, double divisor) { return Quantity(q.value_ / divisor); }
    friend constexpr double operator/(Quantity lhs, Quantity rhs) { return lhs.value_ / rhs.value_; }
    constexpr Quantity& operator+=(Quantity other) { value_ += other.value_; return *this; }
    constexpr Quantity& operator-=(Quantity other) { value_ -= other.value_; return *this; }

    constexpr auto operator<=>(const Quantity&) const = default;

private:
    double value_{0.0};
};

struct MetresTag {};
struct HertzTag {};
struct SecondsTag {};
struct KelvinTag {};
struct PascalTag {};
struct WattTag {};
struct WattHourTag {};
struct RadiansTag {};
struct MetresPerSecondTag {};
struct KilogramsPerCubicMetreTag {};
struct DecibelTag {};
struct DecibelIsotropicTag {};

using Metres = Quantity<MetresTag>;
using Hertz = Quantity<HertzTag>;
using Seconds = Quantity<SecondsTag>;
using Kelvin = Quantity<KelvinTag>;
using Pascal = Quantity<PascalTag>;
using Watt = Quantity<WattTag>;
using WattHour = Quantity<WattHourTag>;
using Radians = Quantity<RadiansTag>;
using MetresPerSecond = Quantity<MetresPerSecondTag>;
using KilogramsPerCubicMetre = Quantity<KilogramsPerCubicMetreTag>;
/// A power ratio in decibels (a loss or a gain).
using Decibel = Quantity<DecibelTag>;
/// Antenna gain relative to an isotropic radiator.
using DecibelIsotropic = Quantity<DecibelIsotropicTag>;

/// Absolute power level referenced to 1 mW. Kept separate from Decibel: a level plus a
/// ratio is a level, a level minus a level is a ratio, and two levels cannot be added.
class DecibelMilliwatt {
public:
    constexpr DecibelMilliwatt() = default;
    constexpr explicit DecibelMilliwatt(double value) : value_(value) {}

    [[nodiscard]] constexpr double value() const { return value_; }

    friend constexpr DecibelMilliwatt operator+(DecibelMilliwatt level, Decibel gain) {
        return DecibelMilliwatt(level.value_ + gain.value());
    }
    friend constexpr DecibelMilliwatt operator-(DecibelMilliwatt level, Decibel loss) {
        return DecibelMilliwatt(level.value_ - loss.value());
    }
    friend constexpr Decibel operator-(DecibelMilliwatt lhs, DecibelMilliwatt rhs) { return Decibel(lhs.value_ - rhs.value_); }
    friend constexpr DecibelMilliwatt operator+(DecibelMilliwatt level, DecibelIsotropic gain) {
        return DecibelMilliwatt(level.value_ + gain.value());
    }

    constexpr auto operator<=>(const DecibelMilliwatt&) const = default;

    [[nodiscard]] static DecibelMilliwatt fromWatts(Watt power) {
        return DecibelMilliwatt(10.0 * std::log10(power.value()) + 30.0);
    }
    [[nodiscard]] Watt toWatts() const { return Watt(std::pow(10.0, (value_ - 30.0) / 10.0)); }

private:
    double value_{0.0};
};

namespace literals {
consteval Metres operator""_m(long double v) { return Metres(static_cast<double>(v)); }
consteval Metres operator""_m(unsigned long long v) { return Metres(static_cast<double>(v)); }
consteval Metres operator""_km(long double v) { return Metres(static_cast<double>(v) * 1000.0); }
consteval Metres operator""_km(unsigned long long v) { return Metres(static_cast<double>(v) * 1000.0); }
consteval Hertz operator""_MHz(long double v) { return Hertz(static_cast<double>(v) * 1e6); }
consteval Hertz operator""_MHz(unsigned long long v) { return Hertz(static_cast<double>(v) * 1e6); }
consteval Hertz operator""_GHz(long double v) { return Hertz(static_cast<double>(v) * 1e9); }
consteval Hertz operator""_GHz(unsigned long long v) { return Hertz(static_cast<double>(v) * 1e9); }
consteval Decibel operator""_dB(long double v) { return Decibel(static_cast<double>(v)); }
consteval Decibel operator""_dB(unsigned long long v) { return Decibel(static_cast<double>(v)); }
consteval DecibelMilliwatt operator""_dBm(long double v) { return DecibelMilliwatt(static_cast<double>(v)); }
consteval DecibelMilliwatt operator""_dBm(unsigned long long v) { return DecibelMilliwatt(static_cast<double>(v)); }
}  // namespace literals

}  // namespace rfcore
