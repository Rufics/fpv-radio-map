#pragma once

// Backup and restore of everything a server cannot rebuild: the rows of its tables and the files
// of its installed modules. A backup holds data only, never schema: it names the migrations its
// rows were written under, and a restore is one SchemaMigrator::migrateThrough (migrations.hpp):
// this server's own steps up to that version, the rows and files loaded there, then the steps
// that came after. Changing the data for a new version is the job of those steps, so a backup
// taken by an older server restores into a newer one, on any PostgreSQL version.
//
// The format is JSON Lines, one object per line (format 2; a restore reads format 1 too):
//   {"format":"rfmap-backup","version":2,"createdAt":"...","migrations":[{"name":"001_init.sql","checksum":"..."}]}
//   {"table":"public.missions","columns":["id",...],"rows":2}
//   ["m1","\\x0102",null,...]                  one array per row, each value in PostgreSQL text form
//   {"file":"modules/x/lib/x.so","mode":420,"size":1234,"sha256":"...","data":"<base64>"}
//   {"end":true,"tables":7,"rows":120,"files":3}

#include "command_line.hpp"
#include "config.hpp"
#include "database.hpp"
#include "migrations.hpp"
#include "staged_files.hpp"

#include <cstddef>
#include <filesystem>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rfmap {

/// A backup that cannot be written or read. A restore that throws it commits nothing and leaves no
/// files behind (MigrationError and StagedFiles::InstallError say the rest).
class BackupError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// What a backup or a restore went through.
struct BackupSummary {
    std::size_t tables = 0;
    std::size_t rows = 0;
    std::size_t files = 0;
    /// For a restore: the migrations applied after the rows were loaded.
    std::vector<std::string> newerMigrations;
};

/// Writes a backup from one consistent snapshot, so the server may keep running meanwhile.
class BackupWriter {
public:
    /// The format it writes.
    static constexpr int kVersion = 2;

    BackupWriter(pqxx::connection& connection, ServerFiles files, std::ostream& log)
        : connection_(connection), files_(std::move(files)), log_(log) {}

    BackupSummary write(std::ostream& out);

private:
    struct Table {
        std::string schema;
        std::string name;
        /// Partitioned: its rows live in its partitions and are read through it.
        bool partitioned = false;
        std::vector<std::string> columns;
    };

    /// The tables to back up, parents before the tables whose foreign keys point at them.
    static std::vector<Table> tables(pqxx::transaction_base& w);
    static std::size_t writeRows(pqxx::transaction_base& w, const Table& table, std::ostream& out);
    std::size_t writeFiles(std::ostream& out) const;
    static void writeFile(const std::string& name, const std::filesystem::path& file, std::ostream& out);

    pqxx::connection& connection_;
    ServerFiles files_;
    std::ostream& log_;
};

/// Reads a backup line by line and brings an older format up to this server's, so the restore
/// reads only the newest format. A newer format is refused.
class BackupReader {
public:
    /// Reads and checks the header.
    explicit BackupReader(std::istream& in);

    [[nodiscard]] const json& header() const { return header_; }
    /// The next line; a backup that ends before it, or a damaged line, is refused.
    json next(std::string_view expected);

private:
    struct Upgrade {
        json (*header)(json);
        json (*line)(json);
    };
    static const std::vector<Upgrade>& upgrades();

    std::istream& in_;
    json header_;
    int format_ = 0;
};

/// Restores a backup into a new, empty database, in one transaction.
class BackupRestorer {
public:
    BackupRestorer(pqxx::connection& connection, const MigrationCatalog& catalog, ServerFiles files,
                   MigrationContext::Note note)
        : connection_(connection), catalog_(catalog), files_(std::move(files)), note_(std::move(note)) {}

    BackupSummary restore(std::istream& in);

private:
    /// The rows and files, on the schema the backup was written under.
    BackupSummary load(MigrationContext& ctx, BackupReader& reader) const;
    /// Refuses a backup whose end line counts other tables, rows or files than it holds.
    static void checkTotals(const json& end, const BackupSummary& summary);
    static std::size_t readRows(pqxx::work& w, const json& table, BackupReader& reader);
    /// Stages a file of the backup next to its place; it moves into place once the database has committed.
    void readFile(const json& entry, StagedFiles& staged) const;
    [[nodiscard]] std::filesystem::path target(const std::string& name) const;

    pqxx::connection& connection_;
    const MigrationCatalog& catalog_;
    ServerFiles files_;
    MigrationContext::Note note_;
};

/// rfmap-server --backup, --restore and --migrate: the configuration and the database of the command line.
class ServiceCommand {
public:
    /// Runs the service action of `cmd`; returns the process exit code (0 done, 1 failed).
    static int run(const CommandLine& cmd, const Config::Environment& env, std::istream& in, std::ostream& out,
                   std::ostream& err);

private:
    static void backup(pqxx::connection& c, const ServerFiles& files, std::ostream& out, std::ostream& err);
    static void restore(pqxx::connection& c, const MigrationCatalog& catalog, const ServerFiles& files,
                        std::istream& in, const MigrationContext::Note& note);
    /// Applies every step, breaking ones too; with `planOnly`, prints them instead.
    static void migrate(pqxx::connection& c, const MigrationCatalog& catalog, const ServerFiles& files, bool planOnly,
                        std::ostream& out, const MigrationContext::Note& note);
};

}  // namespace rfmap
