#pragma once

#include "invocation_scope.hpp"

#include <rfcore/geodesy.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rfmap {

/// A host call the server cannot answer; what() is the message the module receives.
class HostCallError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Reads a host call's request, refusing anything that is not the shape and range the method
/// takes. Every reading names the field, so a module author sees what was wrong.
class HostRequest {
public:
    explicit HostRequest(const json& request);

    [[nodiscard]] const json& raw() const { return request_; }
    [[nodiscard]] bool has(std::string_view field) const;
    /// A finite number, within [min, max].
    [[nodiscard]] double number(std::string_view field, double min, double max) const;
    [[nodiscard]] std::optional<double> optionalNumber(std::string_view field, double min, double max) const;
    [[nodiscard]] std::string string(std::string_view field, std::size_t maxLength = 256) const;
    /// {lat, lon} within the world.
    [[nodiscard]] rfcore::GeoPoint point(std::string_view field) const;
    [[nodiscard]] const json& object(std::string_view field) const;
    /// An ISO 8601 time, or `fallback` when the field is absent.
    [[nodiscard]] TimePoint time(std::string_view field, TimePoint fallback) const;

    static constexpr double kMaxDistanceM = 40'075'000.0;  ///< once around the Earth

private:
    [[nodiscard]] const json& field(std::string_view name) const;
    const json& request_;
};

/// One host method: what the module calls it, and what it answers.
class IHostMethod {
public:
    virtual ~IHostMethod() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    /// Throws HostCallError (or HttpError from the services) with the message for the module.
    [[nodiscard]] virtual json call(const HostRequest& request, const InvocationScope& scope) const = 0;
};

/// The host methods of docs/modules/developer-guide.md section 6, by name.
class HostMethodTable {
public:
    HostMethodTable();
    [[nodiscard]] const IHostMethod* find(std::string_view name) const;
    [[nodiscard]] std::vector<std::string_view> names() const;

private:
    std::vector<std::unique_ptr<IHostMethod>> methods_;
};

}  // namespace rfmap
