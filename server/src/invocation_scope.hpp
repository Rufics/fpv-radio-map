#pragma once

#include "link_service.hpp"

#include <stdexcept>
#include <string>

namespace rfmap {

class Library;
class SchemaRegistry;
class WeatherService;

/// Makes the CalcContext a mission is evaluated with (time, module choices, forecast).
class IContextFactory {
public:
    virtual ~IContextFactory() = default;
    [[nodiscard]] virtual CalcContext contextFor(const json& mission) const = 0;
};

/// What a module's host calls may reach: the services behind docs/modules/developer-guide.md
/// section 6. Handed to the host through the invocation, so the host needs no wiring of its own.
struct HostEnvironment {
    LinkService& links;
    const Library& library;
    WeatherService& weather;
    const SchemaRegistry& schemas;
    const IContextFactory& contexts;
};

/// A module was called where no request is running.
class NoInvocationError : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

/// The request on whose behalf modules are being called on this thread. A module's host call
/// sees the same time, forecast and module choices as the calculation that called it, and the
/// host refuses calls made outside any invocation (a module's init, a stray thread).
///
/// Scopes nest: link.evaluate with a mission opens a new scope for that mission's context, and a
/// module's loss.contribute opens one that names the module, so the host can refuse the
/// recursion of link.evaluate inside loss.contribute. Every scope shares one host-call budget
/// with the request that started the chain.
class InvocationScope {
public:
    static constexpr int kMaxDepth = 4;          ///< host calls nested inside host calls
    static constexpr int kMaxHostCalls = 10000;  ///< host calls per request, over every module

    /// Starts the chain for a request.
    InvocationScope(const HostEnvironment& environment, const CalcContext& context);
    /// A nested scope for `context`, for a host call that evaluates another mission.
    explicit InvocationScope(const CalcContext& context);
    /// A nested scope while the module `contributor` answers loss.contribute.
    explicit InvocationScope(std::string contributor);
    ~InvocationScope();
    InvocationScope(const InvocationScope&) = delete;
    InvocationScope& operator=(const InvocationScope&) = delete;

    /// The innermost scope on this thread, or null outside a request.
    [[nodiscard]] static const InvocationScope* current();
    /// current(), or NoInvocationError.
    [[nodiscard]] static const InvocationScope& require();

    [[nodiscard]] const HostEnvironment& environment() const { return environment_; }
    [[nodiscard]] const CalcContext& context() const { return context_; }
    /// The module contributing loss on this thread, or empty.
    [[nodiscard]] const std::string& contributor() const { return contributor_; }

    /// Counts a host call: the nesting depth of this scope and the request's total.
    /// Throws HostCallBudgetError when either is exhausted.
    class HostCall {
    public:
        explicit HostCall(const InvocationScope& scope);
        ~HostCall();
        HostCall(const HostCall&) = delete;
        HostCall& operator=(const HostCall&) = delete;

    private:
        const InvocationScope& scope_;
    };

private:
    /// The innermost scope the current thread is in; set by the constructors and destructors.
    static const InvocationScope*& currentSlot();
    [[nodiscard]] static const InvocationScope& parent();

    const InvocationScope* previous_;
    const InvocationScope* root_;  ///< the scope that started the chain; holds the budget
    const HostEnvironment& environment_;
    const CalcContext& context_;
    std::string contributor_;
    mutable int depth_ = 0;  ///< host calls open in this scope and its parents
    mutable int calls_ = 0;  ///< on the root only: host calls made in the whole chain
};

/// A host call refused because the request used up its nesting depth or its call budget.
class HostCallBudgetError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace rfmap
