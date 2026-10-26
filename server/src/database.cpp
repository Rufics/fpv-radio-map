#include "database.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <system_error>

namespace rfmap {

Database::Database(std::string url, Settings settings) : url_(std::move(url)), settings_(settings) {
    settings_.poolSize = std::max(1, settings_.poolSize);
    for (int i = 0; i < settings_.poolSize; ++i) idle_.push_back(connect());
}

std::unique_ptr<pqxx::connection> Database::connect() const {
    auto c = std::make_unique<pqxx::connection>(url_);
    applySettings(*c);
    return c;
}

void Database::applySettings(pqxx::connection& c) const {
    if (settings_.statementTimeout.count() > 0) {
        // A number the configuration checked, not text from a request.
        pqxx::nontransaction(c).exec(std::format("SET statement_timeout = {}", settings_.statementTimeout.count()));
    }
}

void Database::Lease::restore() {
    try {
        pqxx::nontransaction(*conn).exec("RESET statement_timeout");
        db.applySettings(*conn);
    } catch (const pqxx::failure& e) {
        // Never back into the pool without its limits: the next lease finds it closed and reconnects.
        db.log_.warn("database", std::string("cannot restore the session settings: ") + e.what());
        conn->close();
    }
}

Database::Lease::Lease(Database& d) : db(d) {
    const auto asked = std::chrono::steady_clock::now();
    std::unique_lock lock(db.mutex_);
    ++db.waiting_;
    const bool leased = db.available_.wait_for(lock, db.settings_.leaseTimeout, [this] { return !db.idle_.empty(); });
    --db.waiting_;
    db.waitSeconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - asked).count();
    if (!leased) db.busy();
    ++db.leases_;
    conn = std::move(db.idle_.back());
    db.idle_.pop_back();
}

void Database::busy() {
    ++leaseTimeouts_;
    throw HttpError(503, "the database is busy; try again in a few seconds", {{"retryAfterS", 2}});
}

DbPoolStats Database::stats() {
    const std::lock_guard lock(mutex_);
    return {settings_.poolSize, static_cast<int>(idle_.size()), waiting_, leases_, waitSeconds_,
            reconnects_.load(), leaseTimeouts_};
}

Database::Lease::~Lease() {
    if (broken || retire || !conn->is_open()) {
        try {
            conn = db.connect();
            if (!retire) db.reconnects_.fetch_add(1);
        } catch (const pqxx::failure& e) {
            // Keep the old one: the next lease fails fast with a clear error instead of hanging.
            db.log_.warn("database", std::string("reconnect failed: ") + e.what());
            if (retire && conn->is_open()) restore();
        }
    }
    try {
        db.giveBack(std::move(conn));
    } catch (const std::system_error& e) {
        // The pool lock failed: the connection is lost and the pool is one smaller from now on.
        db.log_.error("database", std::string("cannot return a connection to the pool: ") + e.what());
    }
}

void Database::giveBack(std::unique_ptr<pqxx::connection> c) {
    {
        const std::lock_guard lock(mutex_);
        // Never reallocates: the connection came out of idle_, whose capacity does not shrink.
        idle_.push_back(std::move(c));
    }
    available_.notify_one();
}

}  // namespace rfmap
