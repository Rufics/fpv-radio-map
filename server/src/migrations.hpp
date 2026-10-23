#pragma once

// The server's schema and how a database moves to it. Every change to the schema or to the data
// for a new version is a migration step: an SQL file in server/sql, or a C++ step for what SQL
// cannot do (rewriting values with the server's own code, or the files beside the database).
// Steps apply in name order, each once, and schema_migrations records each with its checksum.
//
// SchemaMigrator::migrateThrough is the one way a database changes version: the steps up to a
// pivot version, an action on the schema of that version, then the rest. The start of the server
// and --migrate pass no action; a restore passes the backup's version and loads its rows there, so
// the steps after it adapt old data exactly as they adapt a live database.

#include "common.hpp"
#include "database.hpp"
#include "staged_files.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rfmod {
class FileLock;
}  // namespace rfmod

namespace rfmap {

using rfmod::FileLock;

/// A database, a backup or a step that does not fit this server's migrations. Thrown inside the
/// migration transaction, so nothing it would have changed is committed.
class MigrationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// One applied or known migration: its name and a checksum of what it does. An empty checksum is
/// unknown (a row written before checksums, or a backup of format 1): only the name is compared.
struct MigrationId {
    std::string name;
    std::string checksum;
};

/// A schema version as a backup names it: the last step, how many steps lead up to it, and one
/// fingerprint of them all. Alembic and golang-migrate name a version by its last step the same way;
/// the fingerprint adds the check that Flyway makes with each step's checksum.
/// MigrationCatalog::versionAt turns it back into the steps.
struct SchemaStamp {
    std::string version;      ///< the last step's name
    std::size_t steps = 0;    ///< how many steps, the last one included
    std::string fingerprint;  ///< SchemaVersion::fingerprint() of those steps

    /// {"version": ..., "steps": ..., "fingerprint": ...}, as a backup's header holds it.
    [[nodiscard]] json toJson() const;
    static SchemaStamp fromJson(const json& j);
};

/// A schema version: the migrations applied, in order.
class SchemaVersion {
public:
    enum class Relation {
        Same,      ///< the same migrations
        Behind,    ///< a prefix of the other: the other's steps from `at` on are missing here
        Ahead,     ///< the other is a prefix of this: this has steps from `at` on the other lacks
        Diverged,  ///< the two differ at `at`, by name or by checksum
    };
    struct Comparison {
        Relation relation;
        std::size_t at;  ///< the first position where the two differ (the shorter length when one is a prefix)
    };

    SchemaVersion() = default;
    explicit SchemaVersion(std::vector<MigrationId> ids) : ids_(std::move(ids)) {}

    [[nodiscard]] const std::vector<MigrationId>& ids() const { return ids_; }
    [[nodiscard]] std::size_t size() const { return ids_.size(); }
    [[nodiscard]] bool empty() const { return ids_.empty(); }

    /// How this version stands against `other`.
    [[nodiscard]] Comparison compare(const SchemaVersion& other) const;

    /// [{"name": ..., "checksum": ...}, ...], as a backup of format 1 or 2 lists it.
    [[nodiscard]] json toJson() const;
    static SchemaVersion fromJson(const json& j);

    /// SHA-256 hex of every step's name and checksum, in order: one value for the whole history.
    [[nodiscard]] std::string fingerprint() const;
    /// Refuses an empty version and one with an unknown checksum, which no fingerprint covers.
    [[nodiscard]] SchemaStamp stamp() const;
    /// This version with each unknown checksum taken from `known`, where both have the same step
    /// at the same position.
    [[nodiscard]] SchemaVersion withChecksumsFrom(const SchemaVersion& known) const;

private:
    std::vector<MigrationId> ids_;
};

/// What a step sees while it runs: the migration transaction, the server's files and a log.
/// A step never writes a file in place: it stages it, and the file moves into place after the commit.
class MigrationContext {
public:
    using Note = std::function<void(const std::string&)>;

    MigrationContext(pqxx::work& work, const ServerFiles& files, StagedFiles& staged, const Note& note)
        : work_(work), files_(files), staged_(staged), note_(note) {}

    [[nodiscard]] pqxx::work& work() const { return work_; }
    [[nodiscard]] const ServerFiles& files() const { return files_; }
    [[nodiscard]] StagedFiles& staged() const { return staged_; }
    void note(const std::string& line) const {
        if (note_) note_(line);
    }

private:
    pqxx::work& work_;
    const ServerFiles& files_;
    StagedFiles& staged_;
    const Note& note_;
};

/// One step of the schema. A breaking step changes existing data so that an older server can no
/// longer use it: a running server does not apply it on its own (see SchemaMigrator::Options).
class MigrationStep {
public:
    virtual ~MigrationStep() = default;
    MigrationStep(const MigrationStep&) = delete;
    MigrationStep& operator=(const MigrationStep&) = delete;

    [[nodiscard]] const MigrationId& id() const { return id_; }
    [[nodiscard]] bool breaking() const { return breaking_; }
    virtual void apply(MigrationContext& ctx) const = 0;

protected:
    MigrationStep(MigrationId id, bool breaking) : id_(std::move(id)), breaking_(breaking) {}

private:
    MigrationId id_;
    bool breaking_;
};

/// server/sql/NNN_name.sql. Its checksum is the SHA-256 of the file; a first line of
/// `-- rfmap: breaking` marks it breaking.
class SqlMigration final : public MigrationStep {
public:
    static constexpr std::string_view kBreakingMark = "-- rfmap: breaking";

    /// Reads the file now, so a step that cannot be read is refused before anything is applied.
    static std::unique_ptr<SqlMigration> load(const std::filesystem::path& file);

    /// A step of `sql` as read; load() reads it from a file.
    SqlMigration(MigrationId id, bool breaking, std::string sql)
        : MigrationStep(std::move(id), breaking), sql_(std::move(sql)) {}

    void apply(MigrationContext& ctx) const override;

private:
    std::string sql_;
};

/// A step written in C++, named like the SQL files (NNN_name, without .sql) so it sorts among them.
/// Its checksum comes from `revision`: change the revision whenever the step's effect changes.
class CodeMigration : public MigrationStep {
public:
    enum class Breaking { No, Yes };

protected:
    CodeMigration(std::string name, std::string_view revision, Breaking breaking);
};

/// The server's C++ steps. None yet: add a CodeMigration subclass and list it here.
std::vector<std::unique_ptr<MigrationStep>> codeMigrations();

/// Every step this server knows, SQL and C++, in name order.
class MigrationCatalog {
public:
    /// Refuses two steps with one number (the digits before the first '_').
    explicit MigrationCatalog(const std::filesystem::path& sqlDir,
                              std::vector<std::unique_ptr<MigrationStep>> code = codeMigrations());

    [[nodiscard]] const std::vector<std::unique_ptr<MigrationStep>>& steps() const { return steps_; }
    [[nodiscard]] SchemaVersion version() const;
    /// The catalog's steps up to `stamp`'s. Refuses a stamp past them (a newer server's) and one
    /// whose steps differ from the catalog's.
    [[nodiscard]] SchemaVersion versionAt(const SchemaStamp& stamp) const;

private:
    std::vector<std::unique_ptr<MigrationStep>> steps_;
};

/// What a migration does: the version the database records and the steps to apply, before the
/// pivot's action and after it.
struct MigrationPlan {
    SchemaVersion current;
    std::vector<const MigrationStep*> toPivot;
    std::vector<const MigrationStep*> afterPivot;
};

/// The steps a migration applied, by name, before the pivot's action and after it.
struct MigrationReport {
    std::vector<std::string> toPivot;
    std::vector<std::string> afterPivot;
};

/// Moves a database to the catalog's version, in one transaction.
class SchemaMigrator {
public:
    struct Options {
        /// Applies breaking steps to a database that has data. --migrate and a restore pass true; the
        /// start of the server passes false, so a breaking step waits for an administrator's backup.
        bool allowBreaking = false;
        /// Refuses a database that has a schema or any table of its own (a restore).
        bool newDatabaseOnly = false;
        /// What the pivot is, for the messages that refuse it ("the backup").
        std::string_view pivotName = "the pivot version";
    };
    /// The action at the pivot; it runs inside the migration transaction.
    using Action = std::function<void(MigrationContext&)>;

    SchemaMigrator(const MigrationCatalog& catalog, ServerFiles files, MigrationContext::Note note)
        : catalog_(catalog), files_(std::move(files)), note_(std::move(note)) {}

    /// Applies the steps up to `pivot` (none when it is empty or the database is past it), runs
    /// `atPivot` on that schema, applies the rest and commits; then the staged files move into place.
    /// A backup waits for all of it (the module root's lock). With no action and nothing to apply it
    /// changes nothing and takes no file lock.
    /// Refuses, before changing anything, a database or pivot that is ahead of the catalog or
    /// differs from it, and a breaking step the options do not allow.
    MigrationReport migrateThrough(pqxx::connection& c, const SchemaVersion& pivot, const Action& atPivot,
                                   Options options) const;
    MigrationReport migrate(pqxx::connection& c, Options options) const {
        return migrateThrough(c, SchemaVersion(), Action(), options);
    }

    /// What migrateThrough would apply, without changing anything.
    [[nodiscard]] MigrationPlan plan(pqxx::connection& c, const SchemaVersion& pivot, Options options) const;

    /// The version `w` records, read without the migration lock (any transaction, read-only too).
    static SchemaVersion recorded(pqxx::transaction_base& w);

private:
    /// Takes the migration lock for the rest of `w` and creates schema_migrations when missing.
    static void lock(pqxx::work& w);
    [[nodiscard]] MigrationPlan planFor(pqxx::work& w, const SchemaVersion& pivot, Options options) const;
    static void requireNewDatabase(pqxx::work& w, const SchemaVersion& current);
    void requireAllowed(const MigrationPlan& plan, Options options) const;
    std::vector<std::string> apply(MigrationContext& ctx, const std::vector<const MigrationStep*>& steps) const;
    /// True when the database records the catalog's version with every checksum, so there is
    /// nothing to write: a start then does not wait for a running backup.
    /// Only reads: it takes no lock and runs no DDL.
    [[nodiscard]] bool upToDate(pqxx::connection& c) const;
    /// The module root's lock, exclusive, so a backup never reads the rows of one version and the
    /// files of another; none when the migrator has no files.
    [[nodiscard]] std::unique_ptr<FileLock> lockFiles() const;
    /// Fills the checksums that rows written before checksums lack.
    void recordChecksums(pqxx::work& w, const SchemaVersion& current) const;

    const MigrationCatalog& catalog_;
    ServerFiles files_;
    MigrationContext::Note note_;
};

}  // namespace rfmap
