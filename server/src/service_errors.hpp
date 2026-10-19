#pragma once

#include <stdexcept>

namespace rfmap {

/// A broken internal precondition: a caller used a service in a way the code never should.
class ServiceInvariantError : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

/// A schema file the server ships could not be read.
class SchemaFileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// libsodium could not initialise or could not complete an operation.
class CryptoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A database row that is missing or does not hold what the server wrote there.
class StoredDataError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The forecast service could not be reached or refused the request.
class ForecastUnavailableError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace rfmap
