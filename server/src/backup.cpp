#include "backup.hpp"

#include "backup_stream.hpp"
#include "file_lock.hpp"
#include "security.hpp"

#include <sodium.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>

namespace rfmap {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kFormat = "rfmap-backup";
constexpr std::string_view kModulesPrefix = "modules/";
constexpr std::string_view kTrustStoreName = "trust-store";
template <class V>
using ByName = std::map<std::string, V, std::less<>>;
using NameSet = std::set<std::string, std::less<>>;

/// Rows fetched per round trip while writing.
constexpr int kFetchRows = 1000;

/// Tables a backup leaves out: schema_migrations is the header's "migrations", and result_cache
/// is a cache keyed by the server version, so its rows would not be used after a restore anyway.
const NameSet kNotBackedUp = {"public.schema_migrations", "public.result_cache"};

/// Application tables: every table outside the system schemas that no extension owns (PostGIS
/// keeps spatial_ref_sys and the topology and tiger tables, which the extensions create themselves).
constexpr std::string_view kTablesSql = R"(
    select c.oid::bigint, n.nspname, c.relname, c.relkind
    from pg_class c join pg_namespace n on n.oid = c.relnamespace
    where c.relkind in ('r', 'p') and not c.relispartition
      and n.nspname <> 'information_schema' and n.nspname not like 'pg\_%'
      and not exists (select 1 from pg_depend d
                      where d.classid = 'pg_class'::regclass and d.objid = c.oid and d.deptype = 'e')
    order by n.nspname, c.relname)";
/// Generated columns are computed again on restore, so they are not written.
constexpr std::string_view kColumnsSql = R"(
    select attname from pg_attribute
    where attrelid = $1::bigint::oid and attnum > 0 and not attisdropped and attgenerated = ''
    order by attnum)";
constexpr std::string_view kForeignKeysSql =
    "select conrelid::bigint, confrelid::bigint from pg_constraint where contype = 'f' and conrelid <> confrelid";

std::string qualified(const std::string& schema, const std::string& name) { return schema + "." + name; }

/// Orders `names` so each comes after every name it points at in `parents` (Kahn's algorithm,
/// alphabetical among equals). Refuses a cycle, which no load order can satisfy.
std::vector<std::string> parentsFirst(const std::vector<std::string>& names,
                                      const ByName<NameSet>& parents) {
    ByName<std::size_t> waitingFor;
    ByName<std::vector<std::string>> children;
    for (const auto& n : names) waitingFor[n] = 0;
    for (const auto& [child, ps] : parents) {
        for (const auto& p : ps) {
            ++waitingFor[child];
            children[p].push_back(child);
        }
    }
    NameSet ready;
    for (const auto& [n, count] : waitingFor) {
        if (count == 0) ready.insert(n);
    }
    std::vector<std::string> order;
    while (!ready.empty()) {
        const std::string next = *ready.begin();
        ready.erase(ready.begin());
        order.push_back(next);
        for (const auto& c : children[next]) {
            if (--waitingFor[c] == 0) ready.insert(c);
        }
    }
    if (order.size() != names.size()) throw BackupError("the foreign keys between the tables form a cycle");
    return order;
}

std::string base64(std::string_view bytes) {
    std::string out(sodium_base64_ENCODED_LEN(bytes.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
    sodium_bin2base64(out.data(), out.size(), reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(),
                      sodium_base64_VARIANT_ORIGINAL);
    out.resize(out.size() - 1);  // the terminating NUL
    return out;
}

std::string unbase64(std::string_view text, std::string_view what) {
    std::string out(text.size() / 4 * 3 + 3, '\0');
    std::size_t length = 0;
    if (sodium_base642bin(reinterpret_cast<unsigned char*>(out.data()), out.size(), text.data(), text.size(), nullptr,
                          &length, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0) {
        throw BackupError(std::format("{}: the data is not base64", what));
    }
    out.resize(length);
    return out;
}

std::string readAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    if (!in) throw BackupError("cannot read " + file.string());
    return s.str();
}

/// The next line of a backup as JSON; a backup that ends early is refused.
json nextLine(std::istream& in, std::string_view expected) {
    std::string line;
    if (!std::getline(in, line)) throw BackupError(std::string("the backup ends before ") + std::string(expected));
    try {
        return json::parse(line);
    } catch (const json::parse_error&) {
        throw BackupError(std::string("the backup is damaged where ") + std::string(expected) + " should be");
    }
}

/// Whether `relative` stays below the folder it is joined to.
bool staysInside(const fs::path& relative) {
    if (relative.empty() || relative.is_absolute()) return false;
    return std::ranges::none_of(relative, [](const fs::path& part) { return part == ".."; });
}

/// Format 1 named the migrations only: [name, ...]. Format 2 adds each one's checksum.
json headerFromFormat1(json header) {
    json migrations = json::array();
    for (const auto& name : header.at("migrations")) migrations.push_back({{"name", name}, {"checksum", ""}});
    header["migrations"] = std::move(migrations);
    return header;
}

/// Format 3 names the version by its last step and a fingerprint. A list with an unknown checksum
/// (format 1, or rows written before checksums) has no fingerprint, so it stays a list.
json headerFromFormat2(json header) {
    const SchemaVersion listed = SchemaVersion::fromJson(header.at("migrations"));
    if (listed.empty() || std::ranges::any_of(listed.ids(), [](const MigrationId& id) { return id.checksum.empty(); })) {
        return header;
    }
    header["schema"] = listed.stamp().toJson();
    header.erase("migrations");
    return header;
}

}  // namespace

const std::vector<BackupReader::Upgrade>& BackupReader::upgrades() {
    // upgrades()[i] reads format i + 1 as format i + 2. A null line function keeps the lines as they are.
    static const std::vector<Upgrade> all = {{headerFromFormat1, nullptr}, {headerFromFormat2, nullptr}};
    return all;
}

BackupReader::BackupReader(std::istream& in) : in_(in) {
    header_ = nextLine(in_, "the header");
    if (header_.value("format", "") != kFormat) throw BackupError("this is not an rfmap backup");
    const json version = header_.value("version", json());
    if (!version.is_number_integer() || version.get<int>() < 1 || version.get<int>() > BackupWriter::kVersion) {
        throw BackupError("unknown backup version " + version.dump() + ": restore it with the same or a newer server");
    }
    format_ = version.get<int>();
    for (int v = format_; v < BackupWriter::kVersion; ++v) header_ = upgrades().at(v - 1).header(std::move(header_));
    header_["version"] = BackupWriter::kVersion;
}

json BackupReader::next(std::string_view expected) {
    json line = nextLine(in_, expected);
    for (int v = format_; v < BackupWriter::kVersion; ++v) {
        if (const auto upgrade = upgrades().at(v - 1).line) line = upgrade(std::move(line));
    }
    return line;
}

BackupSummary BackupWriter::write(std::ostream& out) {
    // Module installs and trust-store changes of a running server wait until the files are read.
    // The root is created when missing, so a server that installs the first module meanwhile waits too.
    fs::create_directories(files_.modulesRoot);
    const FileLock filesLock(files_.modulesRoot / ModuleGate::kLockFile, FileLock::Mode::Shared);
    if (files_.trustStore.has_parent_path()) fs::create_directories(files_.trustStore.parent_path());
    const FileLock keysLock(rfmod::trustStoreLockFile(files_.trustStore), FileLock::Mode::Shared);
    // One snapshot for every table, so the backup is consistent while the server keeps writing.
    pqxx::transaction<pqxx::isolation_level::repeatable_read, pqxx::write_policy::read_only> w(connection_);
    const SchemaVersion version = SchemaMigrator::recorded(w).withChecksumsFrom(catalog_.version());
    if (version.empty()) throw BackupError("the database has no schema yet: nothing to back up");
    out << json{{"format", kFormat}, {"version", kVersion}, {"createdAt", UtcTime::format(Clock::now())},
                {"schema", version.stamp().toJson()}}.dump()
        << '\n';

    BackupSummary summary;
    for (const Table& table : tables(w)) {
        const std::size_t rows = writeRows(w, table, out);
        log_ << "rfmap-server: backup: " << qualified(table.schema, table.name) << ": " << rows << " rows\n";
        ++summary.tables;
        summary.rows += rows;
    }
    summary.files = writeFiles(out);
    out << json{{"end", true}, {"tables", summary.tables}, {"rows", summary.rows}, {"files", summary.files}}.dump()
        << '\n';
    w.commit();
    return summary;
}

std::vector<BackupWriter::Table> BackupWriter::tables(pqxx::transaction_base& w) {
    ByName<Table> byName;
    std::map<std::int64_t, std::string> nameOf;
    for (const auto& row : w.exec(kTablesSql)) {
        const std::string name = qualified(row[1].as<std::string>(), row[2].as<std::string>());
        if (kNotBackedUp.contains(name)) continue;
        Table table{row[1].as<std::string>(), row[2].as<std::string>(), row[3].as<std::string>() == "p", {}};
        for (const auto& c : w.exec(kColumnsSql, pqxx::params{row[0].as<std::int64_t>()})) {
            table.columns.push_back(c[0].as<std::string>());
        }
        nameOf[row[0].as<std::int64_t>()] = name;
        byName.try_emplace(name, std::move(table));
    }
    ByName<NameSet> parents;
    for (const auto& row : w.exec(kForeignKeysSql)) {
        const auto child = nameOf.find(row[0].as<std::int64_t>());
        const auto parent = nameOf.find(row[1].as<std::int64_t>());
        if (child != nameOf.end() && parent != nameOf.end()) parents[child->second].insert(parent->second);
    }
    std::vector<std::string> names;
    names.reserve(byName.size());
    for (const auto& [name, table] : byName) names.push_back(name);
    std::vector<Table> ordered;
    for (const auto& name : parentsFirst(names, parents)) ordered.push_back(std::move(byName.at(name)));
    return ordered;
}

std::size_t BackupWriter::writeRows(pqxx::transaction_base& w, const Table& table, std::ostream& out) {
    const auto& c = w.conn();
    std::string columns;
    for (const auto& name : table.columns) columns += (columns.empty() ? "" : ", ") + c.quote_name(name);
    // A partitioned table holds no rows of its own: its partitions do, read through it. A plain
    // table is read without its inheritance children, which are backed up as tables of their own.
    const std::string path = (table.partitioned ? "" : "only ") + c.quote_table({table.schema, table.name});
    const std::size_t rows = w.query_value<std::size_t>("select count(*) from " + path);
    out << json{{"table", qualified(table.schema, table.name)}, {"columns", table.columns}, {"rows", rows}}.dump()
        << '\n';
    // Values in their PostgreSQL text form, as the server's own output functions write them, so
    // every type (bytea, jsonb, PostGIS geography) reads back exactly.
    w.exec(std::format("declare backup_rows no scroll cursor for select {} from {}", columns, path));
    std::size_t written = 0;
    for (;;) {
        const pqxx::result batch = w.exec(std::format("fetch forward {} from backup_rows", kFetchRows));
        if (batch.empty()) break;
        for (const auto& row : batch) {
            json values = json::array();
            for (const auto& field : row) values.push_back(field.is_null() ? json() : json(field.c_str()));
            out << values.dump() << '\n';
        }
        written += batch.size();
    }
    w.exec("close backup_rows");
    if (written != rows) throw BackupError(qualified(table.schema, table.name) + " changed while it was written");
    return written;
}

std::size_t BackupWriter::writeFiles(std::ostream& out) const {
    std::size_t count = 0;
    if (fs::is_directory(files_.modulesRoot)) {
        std::vector<fs::path> found;
        for (const auto& e : fs::recursive_directory_iterator(files_.modulesRoot)) {
            if (e.is_regular_file() && !e.is_symlink() && e.path().filename() != ModuleGate::kLockFile) {
                found.push_back(e.path());
            }
        }
        std::ranges::sort(found);
        for (const auto& file : found) {
            writeFile(std::string(kModulesPrefix) + fs::relative(file, files_.modulesRoot).generic_string(), file, out);
            ++count;
        }
    }
    if (fs::is_regular_file(files_.trustStore)) {
        writeFile(std::string(kTrustStoreName), files_.trustStore, out);
        ++count;
    }
    log_ << "rfmap-server: backup: " << count << " files\n";
    return count;
}

void BackupWriter::writeFile(const std::string& name, const fs::path& file, std::ostream& out) {
    const std::string bytes = readAll(file);
    const auto mode = static_cast<unsigned>(fs::status(file).permissions() & fs::perms::mask);
    out << json{{"file", name}, {"mode", mode}, {"size", bytes.size()},
                {"sha256", Secrets::hex(Secrets::sha256(bytes))}, {"data", base64(bytes)}}.dump()
        << '\n';
}

BackupSummary BackupRestorer::restore(std::istream& in) {
    BackupReader reader(in);
    const SchemaVersion written = writtenUnder(reader.header());
    if (written.empty()) throw BackupError("the backup names no migrations");
    BackupSummary summary;
    // The schema the rows were written under, from this server's own migrations; then the rows and
    // files; then the migrations that came after, which adapt the restored rows to this version.
    const MigrationReport report = SchemaMigrator(catalog_, files_, note_).migrateThrough(
        connection_, written, [this, &reader, &summary](MigrationContext& ctx) { summary = load(ctx, reader); },
        {.allowBreaking = true, .newDatabaseOnly = true, .pivotName = "the backup"});
    summary.newerMigrations = report.afterPivot;
    return summary;
}

SchemaVersion BackupRestorer::writtenUnder(const json& header) const {
    if (header.contains("schema")) return catalog_.versionAt(SchemaStamp::fromJson(header.at("schema")));
    return SchemaVersion::fromJson(header.at("migrations"));  // an older format with an unknown checksum
}

BackupSummary BackupRestorer::load(MigrationContext& ctx, BackupReader& reader) const {
    ctx.work().exec("set constraints all deferred");
    BackupSummary summary;
    NameSet destinations;
    json entry = reader.next("the end");
    for (; !entry.contains("end"); entry = reader.next("the end")) {
        if (entry.contains("table")) {
            summary.rows += readRows(ctx.work(), entry, reader);
            ++summary.tables;
        } else if (entry.contains("file")) {
            if (const auto name = entry.at("file").get<std::string>();
                !destinations.insert(target(name).lexically_normal().string()).second) {
                throw BackupError("the backup holds " + name + " twice");
            }
            readFile(entry, ctx.staged());
            ++summary.files;
        } else {
            throw BackupError("the backup holds a line this server does not know: " + entry.dump().substr(0, 80));
        }
    }
    checkTotals(entry, summary);
    return summary;
}

void BackupRestorer::checkTotals(const json& end, const BackupSummary& summary) {
    const json held = {{"tables", summary.tables}, {"rows", summary.rows}, {"files", summary.files}};
    for (const auto& [key, count] : held.items()) {
        if (end.value(key, json()) != count) {
            throw BackupError(std::format("the backup ends with {} {}, but holds {}", end.value(key, json()).dump(),
                                          key, count.dump()));
        }
    }
}

std::size_t BackupRestorer::readRows(pqxx::work& w, const json& table, BackupReader& reader) {
    const auto name = table.at("table").get<std::string>();
    const auto dot = name.find('.');
    if (dot == std::string::npos) throw BackupError("table " + name + " has no schema");
    const auto columns = table.at("columns").get<std::vector<std::string>>();
    const auto rows = table.at("rows").get<std::size_t>();
    const auto& c = w.conn();
    const std::string_view qualifiedName(name);
    auto stream = pqxx::stream_to::raw_table(w, c.quote_table({qualifiedName.substr(0, dot), qualifiedName.substr(dot + 1)}),
                                             c.quote_columns(columns));
    std::vector<std::optional<std::string>> values;
    for (std::size_t i = 0; i < rows; ++i) {
        const std::string where = std::format("row {} of {}", i + 1, name);
        const json row = reader.next(where);
        if (!row.is_array() || row.size() != columns.size()) throw BackupError(where + " does not match its columns");
        values.clear();
        for (const auto& v : row) values.push_back(v.is_null() ? std::nullopt : std::optional(v.get<std::string>()));
        stream.write_row(values);
    }
    stream.complete();
    return rows;
}

void BackupRestorer::readFile(const json& entry, StagedFiles& staged) const {
    const auto name = entry.at("file").get<std::string>();
    const std::string bytes = unbase64(entry.at("data").get<std::string>(), name);
    if (bytes.size() != entry.at("size").get<std::size_t>() ||
        Secrets::hex(Secrets::sha256(bytes)) != entry.at("sha256").get<std::string>()) {
        throw BackupError(name + " does not match its checksum");
    }
    staged.write(target(name), bytes, static_cast<fs::perms>(entry.at("mode").get<unsigned>()));
}

fs::path BackupRestorer::target(const std::string& name) const {
    if (name == kTrustStoreName) return files_.trustStore;
    const fs::path relative = name.starts_with(kModulesPrefix) ? fs::path(name.substr(kModulesPrefix.size())) : fs::path();
    if (!staysInside(relative)) throw BackupError("the backup names a file outside the module root: " + name);
    // A folder already under the root may be a symbolic link that leads out of it, or back to the root.
    const fs::path root = fs::weakly_canonical(files_.modulesRoot);
    const fs::path folder = fs::weakly_canonical((files_.modulesRoot / relative).parent_path());
    if (folder != root && !staysInside(folder.lexically_relative(root))) {
        throw BackupError("the backup's file " + name + " would land outside the module root, through " + folder.string());
    }
    // Replacing the lock file would leave the restore holding a lock on a file no one else opens.
    if (folder == root && relative.filename() == fs::path(ModuleGate::kLockFile)) {
        throw BackupError("the backup names the module lock file: " + name);
    }
    return files_.modulesRoot / relative;
}

int ServiceCommand::run(const CommandLine& cmd, const Config::Environment& env, std::istream& in, std::ostream& out,
                        std::ostream& err) {
    try {
        Secrets::init();
        const Config config = Config::load(cmd.configFile(), env);
        pqxx::connection connection(config.database.url);
        const ServerFiles files{config.modules.root, config.modules.trustStore};
        const MigrationContext::Note note = [&err](const std::string& line) { err << "rfmap-server: " << line << '\n'; };
        switch (cmd.action()) {
            using enum CommandLine::Action;
            case Backup: backup(connection, MigrationCatalog(cmd.sqlDir()), files, out, err); break;
            case Restore: restore(connection, MigrationCatalog(cmd.sqlDir()), files, in, note); break;
            case Migrate: migrate(connection, MigrationCatalog(cmd.sqlDir()), files, cmd.planOnly(), out, note); break;
            case Serve:
            case Health:
            case Help:
            case Invalid: break;
        }
        return 0;
    } catch (const BackupError& e) {
        err << "rfmap-server: " << e.what() << '\n';
    } catch (const MigrationError& e) {
        err << "rfmap-server: " << e.what() << '\n';
    } catch (const StagedFiles::InstallError& e) {
        err << "rfmap-server: " << e.what() << '\n';
    } catch (const pqxx::failure& e) {
        err << "rfmap-server: database: " << e.what() << '\n';
    } catch (const ConfigError& e) {
        err << "rfmap-server: configuration: " << e.what() << '\n';
    } catch (const std::system_error& e) {  // a filesystem_error, or the module lock file
        err << "rfmap-server: " << e.what() << '\n';
    } catch (const json::exception& e) {
        err << "rfmap-server: the backup is damaged: " << e.what() << '\n';
    }
    return 1;
}

void ServiceCommand::backup(pqxx::connection& c, const MigrationCatalog& catalog, const ServerFiles& files,
                            std::ostream& out, std::ostream& err) {
    BackupCompressor packed(out);
    std::ostream file(&packed);
    file.exceptions(std::ios::badbit);  // a failed write throws its own BackupError
    const BackupSummary s = BackupWriter(c, catalog, files, err).write(file);
    packed.finish();
    err << "rfmap-server: backed up " << s.tables << " tables, " << s.rows << " rows and " << s.files << " files\n";
}

void ServiceCommand::restore(pqxx::connection& c, const MigrationCatalog& catalog, const ServerFiles& files,
                             std::istream& in, const MigrationContext::Note& note) {
    BackupDecompressor plain(in);
    std::istream file(&plain);
    file.exceptions(std::ios::badbit);  // a damaged file throws its own BackupError
    const BackupSummary s = BackupRestorer(c, catalog, files, note).restore(file);
    note(std::format("restored {} tables, {} rows and {} files", s.tables, s.rows, s.files));
}

void ServiceCommand::migrate(pqxx::connection& c, const MigrationCatalog& catalog, const ServerFiles& files,
                             bool planOnly, std::ostream& out, const MigrationContext::Note& note) {
    const SchemaMigrator migrator(catalog, files, note);
    const SchemaMigrator::Options options{.allowBreaking = true};
    if (!planOnly) {
        const MigrationReport report = migrator.migrate(c, options);
        note(std::format("the database is at this server's version; applied {} migrations", report.afterPivot.size()));
        return;
    }
    const MigrationPlan plan = migrator.plan(c, SchemaVersion(), options);
    out << "database: " << plan.current.size() << " migrations applied\n";
    if (plan.afterPivot.empty()) out << "nothing to apply\n";
    for (const MigrationStep* s : plan.afterPivot) {
        out << "apply: " << s->id().name << (s->breaking() ? " (breaking)" : "") << '\n';
    }
}

}  // namespace rfmap
