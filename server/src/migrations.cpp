#include "migrations.hpp"

#include "file_lock.hpp"
#include "security.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>

namespace rfmap {

namespace {

namespace fs = std::filesystem;

/// One server migrates a database at a time, even with several replicas starting together.
constexpr long long kMigrationLock = 7461001;

std::string checksumOf(std::string_view bytes) { return Secrets::hex(Secrets::sha256(bytes)); }

/// The digits a step's name starts with: two steps may not share them.
std::string_view numberOf(std::string_view name) { return name.substr(0, name.find('_')); }

std::string nameAt(const SchemaVersion& v, std::size_t at) {
    return at < v.size() ? v.ids()[at].name : std::string("(none)");
}

}  // namespace

SchemaVersion::Comparison SchemaVersion::compare(const SchemaVersion& other) const {
    const std::size_t common = std::min(size(), other.size());
    for (std::size_t i = 0; i < common; ++i) {
        const MigrationId& a = ids_[i];
        const MigrationId& b = other.ids_[i];
        const bool checksumsDiffer = !a.checksum.empty() && !b.checksum.empty() && a.checksum != b.checksum;
        if (a.name != b.name || checksumsDiffer) return {Relation::Diverged, i};
    }
    if (size() == other.size()) return {Relation::Same, common};
    return {size() < other.size() ? Relation::Behind : Relation::Ahead, common};
}

json SchemaVersion::toJson() const {
    json out = json::array();
    for (const auto& id : ids_) out.push_back({{"name", id.name}, {"checksum", id.checksum}});
    return out;
}

SchemaVersion SchemaVersion::fromJson(const json& j) {
    if (!j.is_array()) throw MigrationError("a schema version is a list of migrations");
    std::vector<MigrationId> ids;
    for (const auto& m : j) {
        if (!m.is_object() || !m.contains("name") || !m.at("name").is_string()) {
            throw MigrationError("a schema version names each migration: " + m.dump().substr(0, 80));
        }
        ids.push_back({m.at("name").get<std::string>(), m.value("checksum", std::string())});
    }
    return SchemaVersion(std::move(ids));
}

std::unique_ptr<SqlMigration> SqlMigration::load(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream sql;
    sql << in.rdbuf();
    if (!in) throw MigrationError("cannot read migration " + file.string());
    std::string text = sql.str();
    const std::string_view firstLine = std::string_view(text).substr(0, text.find('\n'));
    const bool breaking = firstLine.starts_with(kBreakingMark);
    MigrationId id{file.filename().string(), checksumOf(text)};
    return std::unique_ptr<SqlMigration>(new SqlMigration(std::move(id), breaking, std::move(text)));
}

void SqlMigration::apply(MigrationContext& ctx) const { ctx.work().exec(sql_); }

CodeMigration::CodeMigration(std::string name, std::string_view revision, Breaking breaking)
    : MigrationStep({std::move(name), checksumOf("code:" + std::string(revision))}, breaking == Breaking::Yes) {}

std::vector<std::unique_ptr<MigrationStep>> codeMigrations() { return {}; }

MigrationCatalog::MigrationCatalog(const fs::path& sqlDir, std::vector<std::unique_ptr<MigrationStep>> code)
    : steps_(std::move(code)) {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(sqlDir)) {
        if (e.is_regular_file() && e.path().extension() == ".sql") files.push_back(e.path());
    }
    for (const auto& f : files) steps_.push_back(SqlMigration::load(f));
    std::ranges::sort(steps_, {}, [](const auto& s) { return s->id().name; });
    const auto same = std::ranges::adjacent_find(
        steps_, [](const auto& a, const auto& b) { return numberOf(a->id().name) == numberOf(b->id().name); });
    if (same != steps_.end()) {
        throw MigrationError(std::format("migrations {} and {} share the number {}", (*same)->id().name,
                                         (*std::next(same))->id().name, numberOf((*same)->id().name)));
    }
}

SchemaVersion MigrationCatalog::version() const {
    std::vector<MigrationId> ids;
    ids.reserve(steps_.size());
    for (const auto& s : steps_) ids.push_back(s->id());
    return SchemaVersion(std::move(ids));
}

SchemaVersion SchemaMigrator::recorded(pqxx::transaction_base& w) {
    if (w.query_value<bool>("select to_regclass('schema_migrations') is null")) return {};
    // to_jsonb reads the checksum of a table written before the column existed too (as null).
    std::vector<MigrationId> ids;
    for (const auto& row : w.exec(R"(select name, coalesce(to_jsonb(m) ->> 'checksum', '')
                                     from schema_migrations m order by name collate "C")")) {
        ids.push_back({row[0].as<std::string>(), row[1].as<std::string>()});
    }
    return SchemaVersion(std::move(ids));
}

void SchemaMigrator::lock(pqxx::work& w) {
    w.exec("select pg_advisory_xact_lock($1)", pqxx::params{kMigrationLock});
    w.exec("create table if not exists schema_migrations (name text primary key, applied_at timestamptz not null "
           "default now(), checksum text)");
    w.exec("alter table schema_migrations add column if not exists checksum text");
}

MigrationPlan SchemaMigrator::planFor(pqxx::work& w, const SchemaVersion& pivot, Options options) const {
    lock(w);
    MigrationPlan plan{recorded(w), {}, {}};
    if (options.newDatabaseOnly) requireNewDatabase(w, plan.current);
    const SchemaVersion target = catalog_.version();
    using enum SchemaVersion::Relation;
    if (const auto c = plan.current.compare(target); c.relation == Ahead) {
        throw MigrationError(std::format("the database has migration {}, which this server does not have: run "
                                         "the same or a newer server",
                                         nameAt(plan.current, c.at)));
    } else if (c.relation == Diverged) {
        throw MigrationError(std::format("the database's migration {} is not this server's {}: a migration was "
                                         "changed after it was applied",
                                         nameAt(plan.current, c.at), nameAt(target, c.at)));
    }
    std::size_t pivotAt = plan.current.size();
    if (!pivot.empty()) {
        if (const auto c = pivot.compare(target); c.relation == Ahead || c.relation == Diverged) {
            throw MigrationError(std::format("{} was written under migration {}, which this server does not have: "
                                             "use the same or a newer server",
                                             options.pivotName, nameAt(pivot, c.at)));
        }
        if (const auto c = plan.current.compare(pivot); c.relation == Ahead || c.relation == Diverged) {
            throw MigrationError(std::format("the database is already past {} (migration {})", options.pivotName,
                                             nameAt(plan.current, c.at)));
        }
        pivotAt = pivot.size();
    }
    const auto& steps = catalog_.steps();
    for (std::size_t i = plan.current.size(); i < steps.size(); ++i) {
        (i < pivotAt ? plan.toPivot : plan.afterPivot).push_back(steps[i].get());
    }
    return plan;
}

void SchemaMigrator::requireNewDatabase(pqxx::work& w, const SchemaVersion& current) {
    if (!current.empty()) throw MigrationError("the database already has a schema: restore needs a new, empty database");
    // Tables of an extension (PostGIS's spatial_ref_sys) come with the extension, not with data.
    const auto found = w.exec(R"(
        select format('%I.%I', n.nspname, c.relname)
        from pg_class c
        join pg_namespace n on n.oid = c.relnamespace
        where n.nspname not like 'pg\_%' and n.nspname <> 'information_schema'
          and c.relkind in ('r', 'p', 'v', 'm', 'f', 'S')
          and (n.nspname, c.relname) <> ('public', 'schema_migrations')
          and not exists (select 1 from pg_depend d
                          where d.classid = 'pg_class'::regclass and d.objid = c.oid and d.deptype = 'e')
        order by 1
        limit 1)");
    if (!found.empty()) {
        throw MigrationError("the database already holds " + found[0][0].as<std::string>() +
                             ": restore needs a new, empty database");
    }
}

void SchemaMigrator::requireAllowed(const MigrationPlan& plan, Options options) const {
    // A new database has no data a breaking step could change.
    if (options.allowBreaking || plan.current.empty()) return;
    for (const auto* steps : {&plan.toPivot, &plan.afterPivot}) {
        for (const MigrationStep* s : *steps) {
            if (s->breaking()) {
                throw MigrationError(std::format("migration {} changes existing data so that an older server cannot "
                                                 "use it: back up with rfmap-server --backup, then apply it with "
                                                 "rfmap-server --migrate",
                                                 s->id().name));
            }
        }
    }
}

void SchemaMigrator::recordChecksums(pqxx::work& w, const SchemaVersion& current) const {
    const auto& steps = catalog_.steps();
    for (std::size_t i = 0; i < current.size(); ++i) {
        if (current.ids()[i].checksum.empty()) {
            w.exec("update schema_migrations set checksum = $2 where name = $1",
                   pqxx::params{steps[i]->id().name, steps[i]->id().checksum});
        }
    }
}

std::vector<std::string> SchemaMigrator::apply(MigrationContext& ctx, const std::vector<const MigrationStep*>& steps) const {
    std::vector<std::string> applied;
    for (const MigrationStep* s : steps) {
        s->apply(ctx);
        ctx.work().exec("insert into schema_migrations (name, checksum) values ($1, $2)",
                        pqxx::params{s->id().name, s->id().checksum});
        ctx.note("applied migration " + s->id().name);
        applied.push_back(s->id().name);
    }
    return applied;
}

std::unique_ptr<FileLock> SchemaMigrator::lockFiles() const {
    if (files_.modulesRoot.empty()) return nullptr;
    fs::create_directories(files_.modulesRoot);
    return std::make_unique<FileLock>(files_.modulesRoot / ModuleGate::kLockFile, FileLock::Mode::Exclusive);
}

MigrationReport SchemaMigrator::migrateThrough(pqxx::connection& c, const SchemaVersion& pivot, const Action& atPivot,
                                               Options options) const {
    // A backup reads the rows and the files under the shared lock: it waits until both have changed.
    const std::unique_ptr<FileLock> filesLock = lockFiles();
    StagedFiles staged;
    pqxx::work w(c);
    const MigrationPlan plan = planFor(w, pivot, options);
    requireAllowed(plan, options);
    recordChecksums(w, plan.current);
    MigrationContext ctx(w, files_, staged, note_);
    MigrationReport report{apply(ctx, plan.toPivot), {}};
    if (atPivot) atPivot(ctx);
    report.afterPivot = apply(ctx, plan.afterPivot);
    if (recorded(w).compare(catalog_.version()).relation != SchemaVersion::Relation::Same) {
        throw MigrationError("after the migration the database records another version than this server's");
    }
    w.commit();
    staged.install();
    return report;
}

MigrationPlan SchemaMigrator::plan(pqxx::connection& c, const SchemaVersion& pivot, Options options) const {
    pqxx::work w(c);  // never committed: the lock and the table it may create go away with it
    return planFor(w, pivot, options);
}

}  // namespace rfmap
