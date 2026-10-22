#include "backup.hpp"

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
/// Restored files are written next to their place under this suffix until the database commits.
constexpr std::string_view kStagedSuffix = ".rfmap-restoring";

/// The files of a restore that wait for its commit: installed together after it, removed when it fails.
class StagedFiles {
public:
    StagedFiles() = default;
    StagedFiles(const StagedFiles&) = delete;
    StagedFiles& operator=(const StagedFiles&) = delete;
    ~StagedFiles() {
        std::error_code ignored;
        for (const auto& file : staged_) fs::remove(file, ignored);
    }

    void add(fs::path staged) { staged_.push_back(std::move(staged)); }
    /// Leaves the files on disk: they are no longer this object's to remove.
    void keep() { staged_.clear(); }

    /// Moves every file to its place; each move replaces the old file in one step. The database
    /// has committed by then, so a file that cannot move stays staged for the administrator.
    void install() {
        for (const auto& file : staged_) {
            const std::string name = file.string();
            std::error_code failed;
            fs::rename(file, name.substr(0, name.size() - kStagedSuffix.size()), failed);
            if (failed) {
                const std::string message =
                    std::format("the database is restored, but {} cannot move into place ({}): move the files "
                                "ending in {} into place by hand",
                                name, failed.message(), kStagedSuffix);
                keep();
                throw BackupError(message);
            }
        }
        keep();
    }

private:
    std::vector<fs::path> staged_;
};
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

}  // namespace

BackupSummary BackupWriter::write(std::ostream& out) {
    // Module installs and trust-store changes of a running server wait until the files are read.
    // The root is created when missing, so a server that installs the first module meanwhile waits too.
    fs::create_directories(files_.modulesRoot);
    const FileLock filesLock(files_.modulesRoot / ModuleGate::kLockFile, FileLock::Mode::Shared);
    // One snapshot for every table, so the backup is consistent while the server keeps writing.
    pqxx::transaction<pqxx::isolation_level::repeatable_read, pqxx::write_policy::read_only> w(connection_);
    std::vector<std::string> migrations;
    for (const auto& row : w.exec(R"(select name from schema_migrations order by name collate "C")")) {
        migrations.push_back(row[0].as<std::string>());
    }
    if (migrations.empty()) throw BackupError("the database has no schema yet: nothing to back up");
    out << json{{"format", kFormat}, {"version", kVersion}, {"createdAt", isoUtc(Clock::now())},
                {"migrations", migrations}}.dump()
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
    const json header = nextLine(in, "the header");
    if (header.value("format", "") != kFormat) throw BackupError("this is not an rfmap backup");
    if (header.value("version", 0) != BackupWriter::kVersion) {
        throw BackupError("unknown backup version " + header.value("version", json()).dump());
    }
    const std::string last = lastCommonMigration(header);

    pqxx::work w(connection_);
    Migrations::Names done = Migrations::lock(w);
    if (!done.empty()) throw BackupError("the database already has a schema: restore needs a new, empty database");
    requireNoTables(w);
    // The schema the rows were written under, from this server's own migrations.
    (void)migrations_.apply(w, done, last);
    w.exec("set constraints all deferred");

    BackupSummary summary;
    StagedFiles staged;
    NameSet destinations;
    json entry = nextLine(in, "the end");
    for (; !entry.contains("end"); entry = nextLine(in, "the end")) {
        if (entry.contains("table")) {
            summary.rows += readRows(w, entry, in);
            ++summary.tables;
        } else if (entry.contains("file")) {
            if (const auto name = entry.at("file").get<std::string>();
                !destinations.insert(target(name).lexically_normal().string()).second) {
                throw BackupError("the backup holds " + name + " twice");
            }
            staged.add(readFile(entry));
            ++summary.files;
        } else {
            throw BackupError("the backup holds a line this server does not know: " + entry.dump().substr(0, 80));
        }
    }
    checkTotals(entry, summary);
    // Then everything that came after, so the migrations adapt the restored rows to this version.
    summary.newerMigrations = migrations_.apply(w, done);
    for (const auto& m : summary.newerMigrations) log_ << "rfmap-server: restore: applied migration " << m << '\n';
    w.commit();
    staged.install();
    return summary;
}

void BackupRestorer::requireNoTables(pqxx::work& w) {
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
        throw BackupError("the database already holds " + found[0][0].as<std::string>() +
                          ": restore needs a new, empty database");
    }
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

std::string BackupRestorer::lastCommonMigration(const json& header) const {
    const auto& theirs = header.at("migrations");
    const auto& ours = migrations_.names();
    if (theirs.empty()) throw BackupError("the backup names no migrations");
    for (std::size_t i = 0; i < theirs.size(); ++i) {
        const auto name = theirs[i].get<std::string>();
        if (i >= ours.size() || ours[i] != name) {
            throw BackupError("the backup was written under migration " + name +
                              ", which this server does not have: restore it with the same or a newer server");
        }
    }
    return theirs.back().get<std::string>();
}

std::size_t BackupRestorer::readRows(pqxx::work& w, const json& table, std::istream& in) {
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
        const json row = nextLine(in, where);
        if (!row.is_array() || row.size() != columns.size()) throw BackupError(where + " does not match its columns");
        values.clear();
        for (const auto& v : row) values.push_back(v.is_null() ? std::nullopt : std::optional(v.get<std::string>()));
        stream.write_row(values);
    }
    stream.complete();
    return rows;
}

fs::path BackupRestorer::readFile(const json& entry) const {
    const auto name = entry.at("file").get<std::string>();
    const std::string bytes = unbase64(entry.at("data").get<std::string>(), name);
    if (bytes.size() != entry.at("size").get<std::size_t>() ||
        Secrets::hex(Secrets::sha256(bytes)) != entry.at("sha256").get<std::string>()) {
        throw BackupError(name + " does not match its checksum");
    }
    const fs::path file = target(name).string() + std::string(kStagedSuffix);
    fs::create_directories(file.parent_path());
    StagedFiles written;  // removes a file that is only partly written
    written.add(file);
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << bytes;
        if (!out.flush()) throw BackupError("cannot write " + file.string());
    }
    fs::permissions(file, static_cast<fs::perms>(entry.at("mode").get<unsigned>()) & fs::perms::mask);
    written.keep();
    return file;
}

fs::path BackupRestorer::target(const std::string& name) const {
    if (name == kTrustStoreName) return files_.trustStore;
    const fs::path relative = name.starts_with(kModulesPrefix) ? fs::path(name.substr(kModulesPrefix.size())) : fs::path();
    if (!staysInside(relative)) throw BackupError("the backup names a file outside the module root: " + name);
    return files_.modulesRoot / relative;
}

int ServiceCommand::run(const CommandLine& cmd, const Config::Environment& env, std::istream& in, std::ostream& out,
                        std::ostream& err) {
    try {
        Secrets::init();
        const Config config = Config::load(cmd.configFile(), env);
        pqxx::connection connection(config.database.url);
        BackupFiles files{config.modules.root, config.modules.trustStore};
        BackupSummary summary;
        if (cmd.action() == CommandLine::Action::Backup) {
            summary = BackupWriter(connection, std::move(files), err).write(out);
            if (!out.flush()) throw BackupError("cannot write the backup to standard output");
        } else {
            const Migrations migrations(cmd.sqlDir());
            summary = BackupRestorer(connection, migrations, std::move(files), err).restore(in);
        }
        err << "rfmap-server: " << (cmd.action() == CommandLine::Action::Backup ? "backed up " : "restored ")
            << summary.tables << " tables, " << summary.rows << " rows and " << summary.files << " files\n";
        return 0;
    } catch (const BackupError& e) {
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

}  // namespace rfmap
