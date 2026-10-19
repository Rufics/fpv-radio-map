#pragma once

// Dedicated exception types of the RF core. They derive from the standard exception that
// matches their meaning, so callers that catch the standard base keep working.

#include <stdexcept>

namespace rfcore {

/// An iterative numerical method did not reach full precision within its iteration limit.
class ConvergenceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The path geometry admits no solution for the requested construction.
class DegenerateGeometryError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace rfcore
