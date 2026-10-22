#pragma once

// The distribution's libpqxx 7.8 is built as C++17, so its exception constructors take no
// std::source_location. Compiled as C++20, its headers would declare ones that take it and
// the link fails; hide the feature so the headers match the library's ABI.
#include <version>
#undef __cpp_lib_source_location  // NOSONAR(cpp:S959): a library feature macro <version> defines; see above
#include <pqxx/pqxx>

#include "logger.hpp"
#include "metrics.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rfmap {

/// The server's schema: server/sql/NNN_*.sql, applied in name order, each once, and recorded in
/// schema_migrations. Both the start of the server and a restore build the schema from these files.
class Migrations {
public:
    /// Names of migrations, as schema_migrations records them.
    using Names = std::set<std::string, std::less<>>;

    explicit Migrations(const std::filesystem::path& sqlDir);

    /// The file names, in the order they apply.
    [[nodiscard]] const std::vector<std::string>& names() const { return names_; }

    /// Takes the migration lock for the rest of `w` (one server migrates at a time, even with
    /// several replicas starting together), creates schema_migrations, and returns what it records.
    static Names lock(pqxx::work& w);

    /// Applies, in order, the files not in `done`, up to and including `last` (all when empty),
    /// and records each in schema_migrations and in `done`. Returns the names applied.
    std::vector<std::string> apply(pqxx::work& w, Names& done, std::string_view last = {}) const;

private:
    std::vector<std::filesystem::path> files_;
    std::vector<std::string> names_;
};

/// A fixed pool of PostgreSQL connections. `with` lends one for the duration of a callback and
/// replaces it if it broke, so one dropped connection does not poison the pool.
class Database {
public:
    Database(std::string url, int poolSize);

    template <class F>
    auto with(F&& f) -> decltype(f(std::declval<pqxx::connection&>())) {
        Lease lease(*this);
        try {
            return std::forward<F>(f)(*lease.conn);
        } catch (const pqxx::broken_connection&) {
            lease.broken = true;
            throw;
        }
    }

    /// Runs `f` in a transaction and commits it.
    template <class F>
    auto tx(F&& f) -> decltype(f(std::declval<pqxx::work&>())) {
        return with([&f](pqxx::connection& c) {
            pqxx::work w(c);
            if constexpr (std::is_void_v<decltype(f(w))>) {
                std::forward<F>(f)(w);
                w.commit();
            } else {
                auto r = std::forward<F>(f)(w);
                w.commit();
                return r;
            }
        });
    }

    /// Applies the migrations in `sqlDir` not yet recorded in schema_migrations. Returns the names applied.
    std::vector<std::string> migrate(const std::filesystem::path& sqlDir);

    [[nodiscard]] DbPoolStats stats();

private:
    /// Returns a leased connection to the pool and wakes one waiting lease.
    void giveBack(std::unique_ptr<pqxx::connection> c);

    struct Lease {
        explicit Lease(Database& db);
        ~Lease();
        Database& db;
        std::unique_ptr<pqxx::connection> conn;
        bool broken = false;
    };

    std::string url_;
    int size_ = 0;
    std::mutex mutex_;
    // Guarded by mutex_, except reconnects_, which the lease updates outside the lock.
    int waiting_ = 0;
    std::uint64_t leases_ = 0;
    double waitSeconds_ = 0.0;
    std::atomic<std::uint64_t> reconnects_{0};
    std::condition_variable available_;
    std::vector<std::unique_ptr<pqxx::connection>> idle_;
    Logger log_;
};

}  // namespace rfmap
