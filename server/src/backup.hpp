#pragma once

// Backup and restore of everything a server cannot rebuild: the rows of its tables and the files
// of its installed modules. A backup holds data only, never schema: it names the migrations its
// rows were written under, and a restore builds the schema from this server's own migrations
// (server/sql), loads the rows, then applies the migrations that came after. Changing the data
// for a new version is the job of those migrations, so a backup taken by an older server restores
// into a newer one, on any PostgreSQL version.
//
// The format is JSON Lines, one object per line:
//   {"format":"rfmap-backup","version":1,"createdAt":"...","migrations":["001_init.sql",...]}
//   {"table":"public.missions","columns":["id",...],"rows":2}
//   ["m1","\\x0102",null,...]                  one array per row, each value in PostgreSQL text form
//   {"file":"modules/x/lib/x.so","mode":420,"size":1234,"sha256":"...","data":"<base64>"}
//   {"end":true,"tables":7,"rows":120,"files":3}

#include "command_line.hpp"
#include "config.hpp"
#include "database.hpp"

#include <cstddef>
#include <filesystem>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace rfmap {

/// A backup that cannot be written or restored. A restore that throws it commits nothing and
/// leaves no files behind.
class BackupError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Where the files of a backup live on this server: the module root and the trust store.
struct BackupFiles {
    std::filesystem::path modulesRoot;
    std::filesystem::path trustStore;
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
    static constexpr int kVersion = 1;

    BackupWriter(pqxx::connection& connection, BackupFiles files, std::ostream& log)
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
    BackupFiles files_;
    std::ostream& log_;
};

/// Restores a backup into a database that has no schema yet, in one transaction.
class BackupRestorer {
public:
    BackupRestorer(pqxx::connection& connection, const Migrations& migrations, BackupFiles files, std::ostream& log)
        : connection_(connection), migrations_(migrations), files_(std::move(files)), log_(log) {}

    BackupSummary restore(std::istream& in);

private:
    /// The last of this server's migrations the backup was written under; refuses a backup
    /// written under migrations this server does not have.
    [[nodiscard]] std::string lastCommonMigration(const json& header) const;
    /// Refuses a backup whose end line counts other tables, rows or files than it holds.
    static void checkTotals(const json& end, const BackupSummary& summary);
    static std::size_t readRows(pqxx::work& w, const json& table, std::istream& in);
    /// Writes a file of the backup next to its place; returns the staged file, which the restore
    /// moves into place once the database has committed.
    [[nodiscard]] std::filesystem::path readFile(const json& entry) const;
    [[nodiscard]] std::filesystem::path target(const std::string& name) const;

    pqxx::connection& connection_;
    const Migrations& migrations_;
    BackupFiles files_;
    std::ostream& log_;
};

/// rfmap-server --backup and --restore: the configuration and the database of the command line.
class ServiceCommand {
public:
    /// Runs the service action of `cmd`; returns the process exit code (0 done, 1 failed).
    static int run(const CommandLine& cmd, const Config::Environment& env, std::istream& in, std::ostream& out,
                   std::ostream& err);
};

}  // namespace rfmap
