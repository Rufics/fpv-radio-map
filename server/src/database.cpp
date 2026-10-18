#include "database.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace rfmap {

Database::Database(std::string url, int poolSize) : url_(std::move(url)) {
    for (int i = 0; i < std::max(1, poolSize); ++i) idle_.push_back(std::make_unique<pqxx::connection>(url_));
}

Database::Lease::Lease(Database& d) : db(d) {
    std::unique_lock lock(db.mutex_);
    db.available_.wait(lock, [&] { return !db.idle_.empty(); });
    conn = std::move(db.idle_.back());
    db.idle_.pop_back();
}

Database::Lease::~Lease() {
    if (broken || !conn->is_open()) {
        try {
            conn = std::make_unique<pqxx::connection>(db.url_);
        } catch (...) {
            // Ignored: keep the dead one; the next lease fails fast with a clear error instead of hanging.
        }
    }
    {
        std::lock_guard lock(db.mutex_);
        db.idle_.push_back(std::move(conn));
    }
    db.available_.notify_one();
}

std::vector<std::string> Database::migrate(const std::filesystem::path& sqlDir) {
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(sqlDir)) {
        if (e.is_regular_file() && e.path().extension() == ".sql") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    std::vector<std::string> applied;
    tx([&](pqxx::work& w) {
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
