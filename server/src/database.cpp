#include "database.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

namespace rfmap {

Database::Database(std::string url, int poolSize) : url_(std::move(url)), size_(std::max(1, poolSize)) {
    for (int i = 0; i < size_; ++i) idle_.push_back(std::make_unique<pqxx::connection>(url_));
}

Database::Lease::Lease(Database& d) : db(d) {
    const auto asked = std::chrono::steady_clock::now();
    std::unique_lock lock(db.mutex_);
    ++db.waiting_;
    db.available_.wait(lock, [this] { return !db.idle_.empty(); });
    --db.waiting_;
    ++db.leases_;
    db.waitSeconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - asked).count();
    conn = std::move(db.idle_.back());
    db.idle_.pop_back();
}

DbPoolStats Database::stats() {
    const std::lock_guard lock(mutex_);
    return {size_, static_cast<int>(idle_.size()), waiting_, leases_, waitSeconds_,
            reconnects_.load(std::memory_order_relaxed)};
}

Database::Lease::~Lease() {
    if (broken || !conn->is_open()) {
        try {
            conn = std::make_unique<pqxx::connection>(db.url_);
            db.reconnects_.fetch_add(1, std::memory_order_relaxed);
        } catch (const pqxx::failure& e) {
            // Keep the dead one: the next lease fails fast with a clear error instead of hanging.
            db.log_.warn("database", std::string("reconnect failed: ") + e.what());
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

std::vector<std::string> Database::migrate(const std::filesystem::path& sqlDir) {
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(sqlDir)) {
        if (e.is_regular_file() && e.path().extension() == ".sql") files.push_back(e.path());
    }
    std::ranges::sort(files);
    std::vector<std::string> applied;
    tx([&files, &applied](pqxx::work& w) {
        // One server migrates at a time, even with several replicas starting together.
        w.exec("select pg_advisory_xact_lock(7461001)");
        w.exec("create table if not exists schema_migrations (name text primary key, applied_at timestamptz not null default now())");
        std::set<std::string> done;
        for (const auto& row : w.exec("select name from schema_migrations")) done.insert(row[0].as<std::string>());
        for (const auto& f : files) {
            const std::string name = f.filename().string();
            if (done.contains(name)) continue;
            std::ifstream in(f);
            std::ostringstream sql;
            sql << in.rdbuf();
            w.exec(sql.str());
            w.exec_params("insert into schema_migrations(name) values ($1)", name);
            applied.push_back(name);
        }
    });
    return applied;
}

}  // namespace rfmap
