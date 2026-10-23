// Backup and restore over real databases: the ctest cluster gets databases of its own here, so
// the other integration tests keep theirs. Linked into the integration binary.

#include "backup.hpp"
#include "file_lock.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <vector>

using namespace rfmap;
namespace fs = std::filesystem;

namespace {

/// The server's own migrations: every test database starts at this version.
const SchemaVersion& serverVersion() {
    static const SchemaVersion version = MigrationCatalog(RFMAP_SQL_DIR).version();
    return version;
}

/// A step `after` places past the server's last, so a test's step never takes a real one's number.
std::string nextStep(int after, std::string_view what) {
    const std::string& last = serverVersion().ids().back().name;
    return std::format("{:03}_{}", std::stoi(last.substr(0, last.find('_'))) + after, what);
}

/// The step most tests add.
const std::string& testStep() {
    static const std::string name = nextStep(1, "test.sql");
    return name;
}

std::vector<std::string> serverStepsAnd(std::initializer_list<std::string> more) {
    std::vector<std::string> names;
    for (const auto& id : serverVersion().ids()) names.push_back(id.name);
    names.insert(names.end(), more);
    return names;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

/// Fresh, empty databases next to the test database, and folders for their module files.
class BackupTest : public ::testing::Test {
protected:
    /// What a backup of source() holds: every application table, its rows, and its two files.
    static constexpr std::size_t kSourceTables = 9;
    static constexpr std::size_t kSourceRows = 6;
    static constexpr std::size_t kSourceFiles = 2;

    void SetUp() override {
        const char* url = std::getenv("RFMAP_TEST_DATABASE_URL");
        if (!url) GTEST_SKIP() << "RFMAP_TEST_DATABASE_URL not set (run through ctest)";
        baseUrl_ = url;
        dir_ = rfmap::test::scratchDir("backup");
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }

    /// A new empty database named `name`; returns its URL.
    std::string freshDatabase(const std::string& name) const {
        pqxx::connection admin(baseUrl_);
        pqxx::nontransaction n(admin);
        n.exec("drop database if exists " + admin.quote_name(name) + " with (force)");
        n.exec("create database " + admin.quote_name(name));
        return baseUrl_.substr(0, baseUrl_.rfind('/') + 1) + name;
    }

    [[nodiscard]] ServerFiles files(const std::string& name) const {
        return {dir_ / name / "modules", dir_ / name / "keys.json"};
    }

    /// A migrated database with a row in every kind of table, and two module files.
    std::string source() const {
        const std::string url = freshDatabase("rfmap_backup_source");
        Database db(url, 1);
        rfmap::test::migrate(db, RFMAP_SQL_DIR);
        db.tx([](pqxx::work& w) {
            w.exec(R"(insert into missions (id, owner_key_hash, name, doc, pilot)
                      values ('m1', '\x00ff10', 'Ridge, "north"' || chr(10) || 'tab	end', '{"a": [1, 2.5]}',
                              'SRID=4326;POINT(35.2 48.5)'::geography))");
            w.exec(R"(insert into missions (id, owner_key_hash, name, doc) values ('m2', '\x01', 'Second', '{}'))");
            w.exec(R"(insert into share_links (id, mission_id, token_hash, pin_hash, role, label)
                      values ('l1', 'm1', '\xaa', 'argon', 'view', null))");
            w.exec(R"(insert into sessions (token_hash, mission_id, link_id, role, expires_at)
                      values ('\xbb', 'm1', 'l1', 'view', now() + interval '1 day'))");
            w.exec(R"(insert into settings (key, value) values ('cache', '{"rows": 10}'))");
            w.exec(R"(insert into flight_logs (id, mission_id, file_name, file_sha256, log_index, decoder, duration_s,
                                               normalized, summary)
                      values ('f1', 'm1', 'LOG00001.BBL', 'ab', 1, 'fpv.blackbox', 12.5, '{"t": [0, 0.5]}', '{}'))");
            w.exec(R"(insert into result_cache (input_hash, kind, result, bytes) values ('h', 'path', '{}', 2))");
        });
        const ServerFiles f = files("source");
        fs::create_directories(f.modulesRoot / "fpv.osd" / "lib");
        std::ofstream(f.modulesRoot / "fpv.osd" / "lib" / "osd.so", std::ios::binary) << std::string("\0\x01\x02 lib", 8);
        std::ofstream(f.trustStore) << R"({"keys": []})";
        return url;
    }

    std::string backup(const std::string& url, const ServerFiles& f) const {
        pqxx::connection c(url);
        std::ostringstream out;
        std::ostringstream log;
        const MigrationCatalog catalog(RFMAP_SQL_DIR);
        (void)BackupWriter(c, catalog, f, log).write(out);
        return out.str();
    }

    BackupSummary restore(const std::string& url, const std::string& text, const fs::path& sqlDir = RFMAP_SQL_DIR,
                          const std::string& filesName = "target") const {
        return restore(url, text, MigrationCatalog(sqlDir), filesName);
    }

    BackupSummary restore(const std::string& url, const std::string& text, const MigrationCatalog& catalog,
                          const std::string& filesName = "target") const {
        pqxx::connection c(url);
        std::istringstream in(text);
        return BackupRestorer(c, catalog, files(filesName), {}).restore(in);
    }

    /// Migrates `url` as the start of a server with `catalog` does, or as --migrate with `options`.
    static MigrationReport migrate(const std::string& url, const MigrationCatalog& catalog,
                                   SchemaMigrator::Options options = {}) {
        pqxx::connection c(url);
        return SchemaMigrator(catalog, {}, {}).migrate(c, options);
    }

    static std::string query(const std::string& url, const std::string& sql) {
        pqxx::connection c(url);
        pqxx::work w(c);
        const auto value = w.query_value<std::optional<std::string>>(sql);
        return value.value_or("<null>");
    }

    /// A copy of the server's migrations plus `extra` as the next one, `name`, in a folder of its own.
    fs::path migrationsWith(const std::string& extra, const std::string& name = testStep(),
                            const std::string& folder = "sql-newer") const {
        const fs::path dir = dir_ / folder;
        fs::create_directories(dir);
        for (const auto& e : fs::directory_iterator(RFMAP_SQL_DIR)) {
            fs::copy_file(e.path(), dir / e.path().filename(), fs::copy_options::skip_existing);
        }
        std::ofstream(dir / name) << extra;
        return dir;
    }

    /// The files under `dir`, leaving out the module lock file that migrations and backups share.
    static std::size_t filesUnder(const fs::path& dir) {
        if (!fs::exists(dir)) return 0;
        std::size_t n = 0;
        for (const auto& e : fs::recursive_directory_iterator(dir)) {
            n += e.is_regular_file() && e.path().filename() != ModuleGate::kLockFile ? 1 : 0;
        }
        return n;
    }

    std::string baseUrl_;
    fs::path dir_;
};

constexpr std::string_view kMission = "select concat_ws('|', id, encode(owner_key_hash, 'hex'), name, doc::text, "
                                      "ST_AsText(pilot)) from missions where id = 'm1'";

}  // namespace

TEST_F(BackupTest, RestoresEveryRowAndFileExactly) {
    const std::string from = source();
    const std::string text = backup(from, files("source"));
    EXPECT_EQ(text.find("result_cache"), std::string::npos) << "the cache is not backed up";
    EXPECT_EQ(text.find("schema_migrations\""), std::string::npos);

    const std::string to = freshDatabase("rfmap_backup_target");
    const BackupSummary s = restore(to, text);
    EXPECT_EQ(s.tables, kSourceTables);
    EXPECT_EQ(s.rows, kSourceRows);
    EXPECT_EQ(s.files, kSourceFiles);
    EXPECT_TRUE(s.newerMigrations.empty());

    EXPECT_EQ(query(to, std::string(kMission)), query(from, std::string(kMission)));
    EXPECT_EQ(query(to, "select normalized::text from flight_logs"), R"({"t": [0, 0.5]})");
    EXPECT_EQ(query(to, "select ST_AsText(pilot) from missions where id = 'm1'"), "POINT(35.2 48.5)");
    EXPECT_EQ(query(to, "select (label is null)::text from share_links"), "true");
    EXPECT_EQ(query(to, "select count(*) from sessions"), "1");
    EXPECT_EQ(query(to, "select count(*) from result_cache"), "0");
    EXPECT_EQ(query(to, "select string_agg(name, ',' order by name) from schema_migrations"),
              query(from, "select string_agg(name, ',' order by name) from schema_migrations"));

    const ServerFiles target = files("target");
    EXPECT_EQ(readFile(target.modulesRoot / "fpv.osd" / "lib" / "osd.so"), std::string("\0\x01\x02 lib", 8));
    EXPECT_EQ(readFile(target.trustStore), R"({"keys": []})");
    EXPECT_EQ(filesUnder(dir_ / "target"), kSourceFiles) << "no staged file is left next to the restored ones";
    // And the restored database backs up to the same rows again.
    const std::string again = backup(to, target);
    EXPECT_EQ(again.substr(again.find('\n')), text.substr(text.find('\n')));
}

TEST_F(BackupTest, NewerMigrationsAdaptTheRestoredRows) {
    const std::string text = backup(source(), files("source"));
    const std::string to = freshDatabase("rfmap_backup_target");
    const fs::path sql = migrationsWith("alter table missions add column note text;"
                                        "update missions set note = 'from ' || name;");
    const BackupSummary s = restore(to, text, sql);
    EXPECT_EQ(s.newerMigrations, std::vector<std::string>{testStep()});
    EXPECT_EQ(query(to, "select note from missions where id = 'm2'"), "from Second");
}

TEST_F(BackupTest, RefusesADatabaseThatHasASchema) {
    const std::string from = source();
    const std::string text = backup(from, files("source"));
    EXPECT_THROW((void)restore(from, text, RFMAP_SQL_DIR, "source"), MigrationError);
    EXPECT_EQ(query(from, "select count(*) from missions"), "2");
}

TEST_F(BackupTest, RefusesADatabaseThatHoldsOtherTables) {
    const std::string text = backup(source(), files("source"));
    const std::string to = freshDatabase("rfmap_backup_target");
    {
        pqxx::connection c(to);
        pqxx::nontransaction n(c);
        n.exec("create table leftover (id int)");
    }
    try {
        (void)restore(to, text);
        FAIL() << "restored into a database that holds a table";
    } catch (const MigrationError& e) {
        EXPECT_NE(std::string(e.what()).find("public.leftover"), std::string::npos) << e.what();
    }
    EXPECT_EQ(query(to, "select to_regclass('public.missions')::text"), "<null>");
}

TEST_F(BackupTest, RefusesABackupFromANewerServerAndChangesNothing) {
    const std::string newer = freshDatabase("rfmap_backup_newer");
    (void)migrate(newer, MigrationCatalog(migrationsWith("create table later (id int);")));
    const std::string text = backup(newer, files("newer"));
    const std::string to = freshDatabase("rfmap_backup_target");
    try {
        (void)restore(to, text);
        FAIL() << "restored a backup of an unknown migration";
    } catch (const MigrationError& e) {
        EXPECT_NE(std::string(e.what()).find(testStep()), std::string::npos) << e.what();
    }
    EXPECT_EQ(query(to, "select to_regclass('public.schema_migrations')::text"), "<null>");
}

TEST_F(BackupTest, ATruncatedBackupRestoresNothing) {
    const std::string text = backup(source(), files("source"));
    const std::string cut = text.substr(0, text.rfind('\n', text.size() - 2) + 1);  // without the end line
    const std::string to = freshDatabase("rfmap_backup_target");
    EXPECT_THROW((void)restore(to, cut), BackupError);
    EXPECT_EQ(query(to, "select to_regclass('public.missions')::text"), "<null>");
    EXPECT_EQ(filesUnder(dir_ / "target"), 0u) << "the files it had read are not left behind";
}

TEST_F(BackupTest, RefusesFilesOutsideTheModuleRoot) {
    std::string text = backup(source(), files("source"));
    const auto at = text.find("\"file\":\"modules/fpv.osd");
    ASSERT_NE(at, std::string::npos);
    text.replace(at, std::string("\"file\":\"modules/fpv.osd").size(), "\"file\":\"modules/../../escape");
    const std::string to = freshDatabase("rfmap_backup_target");
    EXPECT_THROW((void)restore(to, text), BackupError);
    EXPECT_FALSE(fs::exists(dir_ / "escape"));
}

TEST_F(BackupTest, RefusesTheModuleLockFile) {
    const std::string text = backup(source(), files("source"));
    const auto at = text.find("\"file\":\"modules/fpv.osd");
    ASSERT_NE(at, std::string::npos);
    const auto end = text.find('"', at + std::string("\"file\":\"").size());
    for (const std::string name : {"modules/.rfmap-lock", "modules/./.rfmap-lock", "modules//.rfmap-lock"}) {
        std::string named = text;
        named.replace(at, end + 1 - at, "\"file\":\"" + name + "\"");
        const std::string to = freshDatabase("rfmap_backup_target");
        EXPECT_THROW((void)restore(to, named), BackupError) << name;
    }
}

TEST_F(BackupTest, RefusesTheModuleLockFileThroughALinkToTheRoot) {
    const std::string text = backup(source(), files("source"));
    const auto at = text.find("\"file\":\"modules/fpv.osd");
    ASSERT_NE(at, std::string::npos);
    const auto end = text.find('"', at + std::string("\"file\":\"").size());
    std::string named = text;
    named.replace(at, end + 1 - at, "\"file\":\"modules/alias/.rfmap-lock\"");
    const ServerFiles target = files("target");
    fs::create_directories(target.modulesRoot);
    fs::create_directory_symlink(target.modulesRoot, target.modulesRoot / "alias");
    const std::string to = freshDatabase("rfmap_backup_target");
    EXPECT_THROW((void)restore(to, named), BackupError);
}

TEST_F(BackupTest, RefusesAModuleFolderThatLinksOutOfTheRoot) {
    const std::string text = backup(source(), files("source"));
    const ServerFiles target = files("target");
    fs::create_directories(target.modulesRoot);
    fs::create_directories(dir_ / "outside");
    fs::create_directory_symlink(dir_ / "outside", target.modulesRoot / "fpv.osd");
    const std::string to = freshDatabase("rfmap_backup_target");
    EXPECT_THROW((void)restore(to, text), BackupError);
    EXPECT_TRUE(fs::is_empty(dir_ / "outside"));
}

TEST_F(BackupTest, CommandLineBacksUpAndRestores) {
    const std::string from = source();
    const ServerFiles f = files("source");
    Config::Environment env{{"RFMAP_DATABASE_URL", from},
                            {"RFMAP__MODULES__ROOT", f.modulesRoot.string()},
                            {"RFMAP__MODULES__TRUSTSTORE", f.trustStore.string()}};
    std::istringstream none;
    std::ostringstream out;
    std::ostringstream err;
    const auto backupCmd = CommandLine::parse({"--backup"}, env, RFMAP_SQL_DIR);
    ASSERT_EQ(ServiceCommand::run(backupCmd, env, none, out, err), 0) << err.str();
    const std::string summary =
        std::format("backed up {} tables, {} rows and {} files", kSourceTables, kSourceRows, kSourceFiles);
    EXPECT_NE(err.str().find(summary), std::string::npos) << err.str();

    const ServerFiles t = files("target");
    env = {{"RFMAP_DATABASE_URL", freshDatabase("rfmap_backup_target")},
           {"RFMAP__MODULES__ROOT", t.modulesRoot.string()},
           {"RFMAP__MODULES__TRUSTSTORE", t.trustStore.string()}};
    const auto restoreCmd = CommandLine::parse({"--restore", "--sql", RFMAP_SQL_DIR}, env, "/nowhere");
    std::istringstream in(out.str());
    err.str("");
    EXPECT_EQ(ServiceCommand::run(restoreCmd, env, in, out, err), 0) << err.str();
    std::istringstream again(out.str());
    err.str("");
    EXPECT_EQ(ServiceCommand::run(restoreCmd, env, again, out, err), 1);
    EXPECT_NE(err.str().find("already has a schema"), std::string::npos) << err.str();
}

TEST_F(BackupTest, CommandLineWritesZstdAndReadsAPlainBackupToo) {
    const std::string from = source();
    const ServerFiles f = files("source");
    Config::Environment env{{"RFMAP_DATABASE_URL", from},
                            {"RFMAP__MODULES__ROOT", f.modulesRoot.string()},
                            {"RFMAP__MODULES__TRUSTSTORE", f.trustStore.string()}};
    std::istringstream none;
    std::ostringstream packed;
    std::ostringstream err;
    ASSERT_EQ(ServiceCommand::run(CommandLine::parse({"--backup"}, env, RFMAP_SQL_DIR), env, none, packed, err), 0)
        << err.str();
    EXPECT_TRUE(packed.str().starts_with("\x28\xb5\x2f\xfd")) << "a zstd frame";
    const std::string plain = backup(from, f);
    EXPECT_LT(packed.str().size(), plain.size());

    const ServerFiles t = files("target");
    env = {{"RFMAP_DATABASE_URL", freshDatabase("rfmap_backup_target")},
           {"RFMAP__MODULES__ROOT", t.modulesRoot.string()},
           {"RFMAP__MODULES__TRUSTSTORE", t.trustStore.string()}};
    const auto restoreCmd = CommandLine::parse({"--restore"}, env, RFMAP_SQL_DIR);
    std::istringstream in(plain);
    std::ostringstream out;
    EXPECT_EQ(ServiceCommand::run(restoreCmd, env, in, out, err), 0) << err.str();
    EXPECT_EQ(query(env.at("RFMAP_DATABASE_URL"), "select count(*) from missions"), "2");
}

TEST_F(BackupTest, CommandLineRefusesADamagedCompressedBackup) {
    const std::string from = source();
    const ServerFiles f = files("source");
    Config::Environment env{{"RFMAP_DATABASE_URL", from},
                            {"RFMAP__MODULES__ROOT", f.modulesRoot.string()},
                            {"RFMAP__MODULES__TRUSTSTORE", f.trustStore.string()}};
    std::istringstream none;
    std::ostringstream packed;
    std::ostringstream err;
    ASSERT_EQ(ServiceCommand::run(CommandLine::parse({"--backup"}, env, RFMAP_SQL_DIR), env, none, packed, err), 0)
        << err.str();
    std::string damaged = packed.str();
    damaged.resize(damaged.size() / 2);

    const ServerFiles t = files("target");
    env = {{"RFMAP_DATABASE_URL", freshDatabase("rfmap_backup_target")},
           {"RFMAP__MODULES__ROOT", t.modulesRoot.string()},
           {"RFMAP__MODULES__TRUSTSTORE", t.trustStore.string()}};
    std::istringstream in(damaged);
    std::ostringstream out;
    err.str("");
    EXPECT_EQ(ServiceCommand::run(CommandLine::parse({"--restore"}, env, RFMAP_SQL_DIR), env, in, out, err), 1);
    EXPECT_NE(err.str().find("rfmap-server: "), std::string::npos) << err.str();
    EXPECT_EQ(query(env.at("RFMAP_DATABASE_URL"), "select to_regclass('public.missions')::text"), "<null>");
}

TEST_F(BackupTest, RefusesABackupWhoseTotalsDoNotMatch) {
    std::string text = backup(source(), files("source"));
    const std::string total = std::format("\"rows\":{}", kSourceRows);
    const auto at = text.find(total);
    ASSERT_NE(at, std::string::npos);
    text.replace(at, total.size(), std::format("\"rows\":{}", kSourceRows + 1));
    const std::string to = freshDatabase("rfmap_backup_target");
    EXPECT_THROW((void)restore(to, text), BackupError);
    EXPECT_EQ(query(to, "select to_regclass('public.missions')::text"), "<null>");
}

TEST_F(BackupTest, ReadsAPartitionedTableThroughItsPartitions) {
    const std::string from = source();
    pqxx::connection c(from);
    pqxx::work w(c);
    w.exec("create table parted (id int) partition by range (id)");
    w.exec("create table parted_low partition of parted for values from (0) to (10)");
    w.exec("insert into parted values (1), (2)");
    w.commit();
    const std::string text = backup(from, files("source"));
    EXPECT_NE(text.find(R"({"columns":["id"],"rows":2,"table":"public.parted"})"), std::string::npos) << text;
    EXPECT_NE(text.find("[\"1\"]\n[\"2\"]\n"), std::string::npos);
    EXPECT_EQ(text.find("parted_low"), std::string::npos) << "a partition is read through its parent";
}

TEST_F(BackupTest, AFileThatFailsHalfWayIsNotLeftBehind) {
    std::string text = backup(source(), files("source"));
    const auto at = text.find("\"file\":\"trust-store\",\"mode\":");
    ASSERT_NE(at, std::string::npos);
    const auto mode = at + std::string("\"file\":\"trust-store\",\"mode\":").size();
    text.replace(mode, text.find(',', mode) - mode, "\"rw\"");
    const std::string to = freshDatabase("rfmap_backup_target");
    EXPECT_ANY_THROW((void)restore(to, text));
    EXPECT_EQ(filesUnder(dir_ / "target"), 0u);
}

TEST_F(BackupTest, AFileThatCannotMoveIntoPlaceStaysStaged) {
    const std::string text = backup(source(), files("source"));
    const ServerFiles target = files("target");
    fs::create_directories(target.trustStore / "in-the-way");
    const std::string to = freshDatabase("rfmap_backup_target");
    try {
        (void)restore(to, text);
        FAIL() << "moved the trust store over a directory";
    } catch (const StagedFiles::InstallError& e) {
        EXPECT_NE(std::string(e.what()).find("into place by hand"), std::string::npos) << e.what();
    }
    EXPECT_EQ(query(to, "select count(*) from missions"), "2") << "the database committed before the move";
    EXPECT_EQ(readFile(target.trustStore.string() + ".rfmap-restoring"), R"({"keys": []})");
}

TEST_F(BackupTest, WaitsForAModuleChangeToFinish) {
    const std::string from = source();
    const ServerFiles f = files("source");
    ModuleGate gate;
    gate.attach(f.modulesRoot);
    auto change = std::make_unique<ModuleGate::Change>(gate);
    auto pending = std::async(std::launch::async, [&] { return backup(from, f); });
    EXPECT_EQ(pending.wait_for(std::chrono::milliseconds(300)), std::future_status::timeout);
    change.reset();
    const std::string text = pending.get();
    EXPECT_NE(text.find("\"end\":true"), std::string::npos);
    EXPECT_EQ(text.find(std::string(ModuleGate::kLockFile)), std::string::npos) << "the lock file is not backed up";
}

TEST_F(BackupTest, RefusesAFileThatComesTwice) {
    std::string text = backup(source(), files("source"));
    const auto start = text.find("{\"data\"");
    ASSERT_NE(start, std::string::npos);
    const std::string file = text.substr(start, text.find('\n', start) + 1 - start);
    text.insert(start, file);
    const auto files = text.find("\"files\":2");
    ASSERT_NE(files, std::string::npos);
    text.replace(files, std::string("\"files\":2").size(), "\"files\":3");
    const std::string to = freshDatabase("rfmap_backup_target");
    try {
        (void)restore(to, text);
        FAIL() << "restored a file twice";
    } catch (const BackupError& e) {
        EXPECT_NE(std::string(e.what()).find("twice"), std::string::npos) << e.what();
    }
    EXPECT_EQ(query(to, "select to_regclass('public.missions')::text"), "<null>");
}

namespace {

/// A C++ step as a future version would add one: it rewrites rows with code and adds a module file.
class UpperNotes final : public CodeMigration {
public:
    UpperNotes() : CodeMigration(nextStep(2, "upper_notes"), "rev 1", Breaking::No) {}
    void apply(MigrationContext& ctx) const override {
        ctx.work().exec("update missions set note = upper(note)");
        ctx.staged().write(ctx.files().modulesRoot / "added.txt", "from a step",
                           fs::perms::owner_read | fs::perms::owner_write);
        // The restored trust store is still staged here: the step reads and rewrites that copy.
        const auto keys = ctx.staged().read(ctx.files().trustStore);
        if (keys) ctx.staged().write(ctx.files().trustStore, *keys + "\n", fs::perms::owner_read | fs::perms::owner_write);
        ctx.note("upper-cased the notes");
    }
};

std::vector<std::unique_ptr<MigrationStep>> upperNotes() {
    std::vector<std::unique_ptr<MigrationStep>> code;
    code.push_back(std::make_unique<UpperNotes>());
    return code;
}

}  // namespace

TEST_F(BackupTest, AnOldBackupIsRestoredAtItsVersionThenTakenThroughEveryNewerStep) {
    // Written by a server that has 001 only; restored by one that also has 002 (SQL) and 003 (C++).
    const std::string text = backup(source(), files("source"));
    const fs::path sql = migrationsWith("alter table missions add column note text;"
                                        "update missions set note = 'from ' || name;",
                                        nextStep(1, "note.sql"));
    const MigrationCatalog newer(sql, upperNotes());
    const std::string to = freshDatabase("rfmap_backup_target");
    const ServerFiles target = files("target");
    pqxx::connection c(to);
    std::istringstream in(text);
    std::vector<std::string> notes;
    const BackupSummary s =
        BackupRestorer(c, newer, target, [&notes](const std::string& line) { notes.push_back(line); }).restore(in);

    const std::string note = nextStep(1, "note.sql");
    const std::string upper = nextStep(2, "upper_notes");
    EXPECT_EQ(s.newerMigrations, (std::vector<std::string>{note, upper}));
    // 002 filled the note from the restored rows, so it ran after them; 003 ran after 002.
    EXPECT_EQ(query(to, "select note from missions where id = 'm2'"), "FROM SECOND");
    EXPECT_EQ(query(to, "select string_agg(name, ',' order by name) from schema_migrations"),
              [&] {
                  std::string all;
                  for (const auto& n : serverStepsAnd({note, upper})) all += (all.empty() ? "" : ",") + n;
                  return all;
              }());
    EXPECT_EQ(query(to, "select count(*) from schema_migrations where checksum is null or checksum = ''"), "0");
    EXPECT_EQ(readFile(target.modulesRoot / "added.txt"), "from a step");
    EXPECT_EQ(readFile(target.trustStore), "{\"keys\": []}\n") << "the step saw the restored file";
    EXPECT_EQ(filesUnder(dir_ / "target"), 3u) << "nothing left staged";
    EXPECT_NE(std::ranges::find(notes, "upper-cased the notes"), notes.end());
}

TEST_F(BackupTest, ABackupOfFormat1StillRestores) {
    const std::string text = backup(source(), files("source"));
    const std::string body = text.substr(text.find('\n'));
    const json header = {{"format", "rfmap-backup"}, {"version", 1}, {"createdAt", "2026-10-21T00:00:00Z"},
                         {"migrations", serverStepsAnd({})}};
    const std::string format1 = header.dump() + body;
    const std::string to = freshDatabase("rfmap_backup_target");
    const BackupSummary s = restore(to, format1);
    EXPECT_EQ(s.rows, kSourceRows);
    EXPECT_EQ(query(to, "select count(*) from missions"), "2");
    EXPECT_EQ(query(to, "select count(*) from schema_migrations where length(checksum) <> 64"), "0")
        << "taken from this server";
}

TEST_F(BackupTest, TheHeaderNamesTheVersionByItsLastStepAndAFingerprint) {
    const std::string text = backup(source(), files("source"));
    const json header = json::parse(text.substr(0, text.find('\n')));
    EXPECT_EQ(header.at("version"), 3);
    EXPECT_FALSE(header.contains("migrations"));
    EXPECT_EQ(header.at("schema").at("version"), serverVersion().ids().back().name);
    EXPECT_EQ(header.at("schema").at("steps"), serverVersion().size());
    EXPECT_EQ(header.at("schema").at("fingerprint"), serverVersion().fingerprint());
}

TEST_F(BackupTest, ABackupOfFormat2StillRestores) {
    const std::string text = backup(source(), files("source"));
    const json header = {{"format", "rfmap-backup"}, {"version", 2}, {"createdAt", "2026-10-21T00:00:00Z"},
                         {"migrations", serverVersion().toJson()}};
    const std::string to = freshDatabase("rfmap_backup_target");
    EXPECT_EQ(restore(to, header.dump() + text.substr(text.find('\n'))).rows, kSourceRows);
    EXPECT_EQ(query(to, "select count(*) from missions"), "2");
}

TEST_F(BackupTest, RefusesABackupFromAnotherHistory) {
    std::string text = backup(source(), files("source"));
    const std::string fingerprint = serverVersion().fingerprint();
    text.replace(text.find(fingerprint), fingerprint.size(), std::string(fingerprint.size(), '0'));
    const std::string to = freshDatabase("rfmap_backup_target");
    try {
        (void)restore(to, text);
        FAIL() << "restored a backup whose steps differ from the server's";
    } catch (const MigrationError& e) {
        EXPECT_NE(std::string(e.what()).find("another history"), std::string::npos) << e.what();
    }
    EXPECT_EQ(query(to, "select to_regclass('public.schema_migrations')::text"), "<null>");
}

TEST_F(BackupTest, ADatabaseMigratedBeforeChecksumsBacksUpWithTheServersChecksums) {
    const std::string from = source();
    {
        pqxx::connection c(from);
        pqxx::work w(c);  // as a server before checksums left it
        w.exec("alter table schema_migrations drop column checksum");
        w.commit();
    }
    const std::string text = backup(from, files("source"));
    EXPECT_NE(text.find(serverVersion().fingerprint()), std::string::npos);
    const std::string to = freshDatabase("rfmap_backup_target");
    EXPECT_EQ(restore(to, text).rows, kSourceRows);
}

TEST_F(BackupTest, RefusesABackupFormatNewerThanTheServer) {
    std::string text = backup(source(), files("source"));
    text.replace(text.find("\"version\":3"), std::string("\"version\":3").size(), "\"version\":4");
    const std::string to = freshDatabase("rfmap_backup_target");
    EXPECT_THROW((void)restore(to, text), BackupError);
}

TEST_F(BackupTest, AStartRefusesADatabaseNewerThanTheServer) {
    const std::string url = freshDatabase("rfmap_backup_target");
    (void)migrate(url, MigrationCatalog(migrationsWith("create table later (id int);")));
    try {
        (void)migrate(url, MigrationCatalog(RFMAP_SQL_DIR));
        FAIL() << "an older server started on a newer database";
    } catch (const MigrationError& e) {
        EXPECT_NE(std::string(e.what()).find(testStep()), std::string::npos) << e.what();
    }
}

TEST_F(BackupTest, AStartRefusesAMigrationChangedAfterItWasApplied) {
    const std::string url = freshDatabase("rfmap_backup_target");
    (void)migrate(url, MigrationCatalog(migrationsWith("create table later (id int);", testStep(), "a")));
    const MigrationCatalog changed(migrationsWith("create table later (id bigint);", testStep(), "b"));
    try {
        (void)migrate(url, changed);
        FAIL() << "started with a changed migration";
    } catch (const MigrationError& e) {
        EXPECT_NE(std::string(e.what()).find("changed after it was applied"), std::string::npos) << e.what();
    }
}

TEST_F(BackupTest, ABreakingStepWaitsForMigrate) {
    const std::string url = source();
    const MigrationCatalog breaking(migrationsWith("-- rfmap: breaking\ndelete from sessions;"));
    try {
        (void)migrate(url, breaking);
        FAIL() << "a start applied a breaking step";
    } catch (const MigrationError& e) {
        EXPECT_NE(std::string(e.what()).find("rfmap-server --migrate"), std::string::npos) << e.what();
    }
    EXPECT_EQ(query(url, "select count(*) from sessions"), "1");
    const MigrationReport r = migrate(url, breaking, {.allowBreaking = true});
    EXPECT_EQ(r.afterPivot, std::vector<std::string>{testStep()});
    EXPECT_EQ(query(url, "select count(*) from sessions"), "0");
    // A new database has no data to break: a start builds it with every step.
    const std::string fresh = freshDatabase("rfmap_backup_target");
    EXPECT_EQ(migrate(fresh, breaking).afterPivot.size(), serverVersion().size() + 1);
}

TEST_F(BackupTest, ChecksumsAreFilledForRowsWrittenBeforeThem) {
    const std::string url = freshDatabase("rfmap_backup_target");
    {
        pqxx::connection c(url);
        pqxx::work w(c);  // as a server before checksums left it
        w.exec("create table schema_migrations (name text primary key, applied_at timestamptz not null default now())");
        const std::string& first = serverVersion().ids().front().name;
        w.exec("insert into schema_migrations (name) values ($1)", pqxx::params{first});
        std::ifstream in(fs::path(RFMAP_SQL_DIR) / first);
        std::ostringstream sql;
        sql << in.rdbuf();
        w.exec(sql.str());
        w.commit();
    }
    const MigrationCatalog catalog(RFMAP_SQL_DIR);
    EXPECT_EQ(migrate(url, catalog).afterPivot.size(), serverVersion().size() - 1);
    EXPECT_EQ(query(url, "select checksum from schema_migrations order by name collate \"C\" limit 1"),
              serverVersion().ids().front().checksum);
}

TEST_F(BackupTest, MigrateCommandPlansThenApplies) {
    const std::string url = source();
    const fs::path sql = migrationsWith("-- rfmap: breaking\ndelete from sessions;");
    const ServerFiles f = files("source");
    Config::Environment env{{"RFMAP_DATABASE_URL", url},
                            {"RFMAP__MODULES__ROOT", f.modulesRoot.string()},
                            {"RFMAP__MODULES__TRUSTSTORE", f.trustStore.string()}};
    std::istringstream none;
    std::ostringstream out;
    std::ostringstream err;
    const auto plan = CommandLine::parse({"--migrate", "--plan", "--sql", sql.string()}, env, "/nowhere");
    ASSERT_EQ(ServiceCommand::run(plan, env, none, out, err), 0) << err.str();
    EXPECT_EQ(out.str(), std::format("database: {} migrations applied\napply: {} (breaking)\n", serverVersion().size(),
                                     testStep()));
    EXPECT_EQ(query(url, "select count(*) from sessions"), "1") << "a plan changes nothing";
    const auto apply = CommandLine::parse({"--migrate", "--sql", sql.string()}, env, "/nowhere");
    ASSERT_EQ(ServiceCommand::run(apply, env, none, out, err), 0) << err.str();
    EXPECT_NE(err.str().find("applied migration " + testStep()), std::string::npos) << err.str();
    EXPECT_EQ(query(url, "select count(*) from sessions"), "0");
}

TEST_F(BackupTest, TwoStepsWithOneNumberAreRefused) {
    class Clash final : public CodeMigration {
    public:
        Clash() : CodeMigration(serverVersion().ids().front().name.substr(0, 4) + "clash", "rev 1", Breaking::No) {}
        void apply(MigrationContext&) const override {}
    };
    std::vector<std::unique_ptr<MigrationStep>> code;
    code.push_back(std::make_unique<Clash>());
    EXPECT_THROW(MigrationCatalog(RFMAP_SQL_DIR, std::move(code)), MigrationError);
}

TEST_F(BackupTest, ABackupWaitsForARestoreToPutItsFilesInPlace) {
    const std::string text = backup(source(), files("source"));
    const ServerFiles target = files("target");
    fs::create_directories(target.modulesRoot);
    auto backupRunning = std::make_unique<FileLock>(target.modulesRoot / ModuleGate::kLockFile, FileLock::Mode::Shared);
    const std::string to = freshDatabase("rfmap_backup_target");
    auto restoring = std::async(std::launch::async, [&] { return restore(to, text); });
    EXPECT_EQ(restoring.wait_for(std::chrono::milliseconds(300)), std::future_status::timeout);
    backupRunning.reset();
    EXPECT_EQ(restoring.get().rows, kSourceRows);
}

TEST_F(BackupTest, AStartWithNothingToApplyDoesNotWaitForABackup) {
    const std::string url = source();
    const ServerFiles f = files("source");
    fs::create_directories(f.modulesRoot);
    const FileLock backupRunning(f.modulesRoot / ModuleGate::kLockFile, FileLock::Mode::Shared);
    // A backup's snapshot transaction holds a read lock on schema_migrations until it ends.
    pqxx::connection backupConnection(url);
    pqxx::transaction<pqxx::isolation_level::repeatable_read, pqxx::write_policy::read_only> snapshot(backupConnection);
    (void)SchemaMigrator::recorded(snapshot);
    auto start = std::async(std::launch::async, [&] {
        pqxx::connection c(url);
        return SchemaMigrator(MigrationCatalog(RFMAP_SQL_DIR), f, {}).migrate(c, {});
    });
    ASSERT_EQ(start.wait_for(std::chrono::seconds(10)), std::future_status::ready) << "the start waited for the backup";
    EXPECT_TRUE(start.get().afterPivot.empty());
}

TEST_F(BackupTest, AFileUnderTheStagedNameIsNeverOverwritten) {
    const fs::path target = dir_ / "modules" / "a.so";
    fs::create_directories(target.parent_path());
    const fs::path other = target.string() + std::string(StagedFiles::kSuffix);
    std::ofstream(other) << "a module's own file";
    {
        StagedFiles staged;
        EXPECT_THROW(staged.write(target, "new", fs::perms::owner_read), fs::filesystem_error);
    }
    EXPECT_EQ(readFile(other), "a module's own file");
    EXPECT_FALSE(fs::exists(target));
}

TEST_F(BackupTest, StagedReadTellsAMissingFileFromAnUnreadableOne) {
    const StagedFiles staged;
    EXPECT_EQ(staged.read(dir_ / "missing"), std::nullopt);
    EXPECT_THROW((void)staged.read(dir_), fs::filesystem_error) << "a directory cannot be read as a file";
    fs::create_symlink(dir_ / "gone", dir_ / "dangling");
    EXPECT_THROW((void)staged.read(dir_ / "dangling"), fs::filesystem_error) << "a dangling link is not a missing file";
}
