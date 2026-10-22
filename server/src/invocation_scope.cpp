#include "invocation_scope.hpp"

#include <format>

namespace rfmap {

const InvocationScope*& InvocationScope::currentSlot() {
    // The one thread-bound value of the server: which request this thread is serving. Every
    // other piece of per-request state hangs off the scope it names.
    thread_local const InvocationScope* current = nullptr;
    return current;
}

const InvocationScope* InvocationScope::current() { return currentSlot(); }

const InvocationScope& InvocationScope::require() {
    if (const InvocationScope* s = current()) return *s;
    throw NoInvocationError("host calls are available only while the server evaluates a request");
}

const InvocationScope& InvocationScope::parent() {
    if (const InvocationScope* s = current()) return *s;
    throw NoInvocationError("a nested invocation needs a request to nest in");
}

InvocationScope::InvocationScope(const HostEnvironment& environment, const CalcContext& context)
    : previous_(currentSlot()), root_(this), environment_(environment), context_(context) {
    currentSlot() = this;
}

InvocationScope::InvocationScope(const CalcContext& context)
    : previous_(&parent()),
      root_(previous_->root_),
      environment_(previous_->environment_),
      context_(context),
      depth_(previous_->depth_) {
    currentSlot() = this;
}

InvocationScope::InvocationScope(std::string contributor)
    : previous_(&parent()),
      root_(previous_->root_),
      environment_(previous_->environment_),
      context_(previous_->context_),
      contributor_(std::move(contributor)),
      depth_(previous_->depth_) {
    currentSlot() = this;
}

InvocationScope::~InvocationScope() { currentSlot() = previous_; }

InvocationScope::HostCall::HostCall(const InvocationScope& scope) : scope_(scope) {
    if (scope.depth_ >= kMaxDepth) {
        throw HostCallBudgetError(std::format("host calls nested deeper than {}", kMaxDepth));
    }
    if (scope.root_->calls_ >= kMaxHostCalls) {
        throw HostCallBudgetError(std::format("this request made more than {} host calls", kMaxHostCalls));
    }
    ++scope.depth_;
    ++scope.root_->calls_;
}

InvocationScope::HostCall::~HostCall() { --scope_.depth_; }

}  // namespace rfmap
