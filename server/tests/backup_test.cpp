// Backup and restore over real databases: the ctest cluster gets databases of its own here, so
// the other integration tests keep theirs. Linked into the integration binary.

#include "backup.hpp"
#include "file_lock.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <future>
#include <sstream>

using namespace rfmap;
namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

/// Fresh, empty databases next to the test database, and folders for their module files.
class BackupTest : public ::testing::Test {
protected:
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

    [[nodiscard]] BackupFiles files(const std::string& name) const {
        return {dir_ / name / "modules", dir_ / name / "keys.json"};
    }

    /// A migrated database with a row in every kind of table, and two module files.
    std::string source() const {
        const std::string url = freshDatabase("rfmap_backup_source");
        Database db(url, 1);
        (void)db.migrate(RFMAP_SQL_DIR);
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
            w.exec(R"(insert into result_cache (input_hash, kind, result, bytes) values ('h', 'path', '{}', 2))");
        });
        const BackupFiles f = files("source");
        fs::create_directories(f.modulesRoot / "fpv.osd" / "lib");
        std::ofstream(f.modulesRoot / "fpv.osd" / "lib" / "osd.so", std::ios::binary) << std::string("\0\x01\x02 lib", 8);
        std::ofstream(f.trustStore) << R"({"keys": []})";
        return url;
    }

    std::string backup(const std::string& url, const BackupFiles& f) const {
        pqxx::connection c(url);
        std::ostringstream out;
        std::ostringstream log;
        (void)BackupWriter(c, f, log).write(out);
        return out.str();
    }

    BackupSummary restore(const std::string& url, const std::string& text, const fs::path& sqlDir = RFMAP_SQL_DIR,
                          const std::string& filesName = "target") const {
        pqxx::connection c(url);
        const Migrations migrations(sqlDir);
        std::istringstream in(text);
        std::ostringstream log;
        return BackupRestorer(c, migrations, files(filesName), log).restore(in);
    }

    static std::string query(const std::string& url, const std::string& sql) {
        pqxx::connection c(url);
        pqxx::work w(c);
        const auto value = w.query_value<std::optional<std::string>>(sql);
        return value.value_or("<null>");
    }

    /// A copy of the server's migrations plus `extra` as the next one.
    fs::path migrationsWith(const std::string& extra) const {
        const fs::path dir = dir_ / "sql-newer";
        fs::create_directories(dir);
        for (const auto& e : fs::directory_iterator(RFMAP_SQL_DIR)) fs::copy_file(e.path(), dir / e.path().filename());
        std::ofstream(dir / "999_test.sql") << extra;
        return dir;
    }

    static std::size_t filesUnder(const fs::path& dir) {
        if (!fs::exists(dir)) return 0;
        std::size_t n = 0;
        for (const auto& e : fs::recursive_directory_iterator(dir)) n += e.is_regular_file() ? 1 : 0;
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
    EXPECT_EQ(s.tables, 8u);
    EXPECT_EQ(s.rows, 5u);
    EXPECT_EQ(s.files, 2u);
    EXPECT_TRUE(s.newerMigrations.empty());

    EXPECT_EQ(query(to, std::string(kMission)), query(from, std::string(kMission)));
    EXPECT_EQ(query(to, "select ST_AsText(pilot) from missions where id = 'm1'"), "POINT(35.2 48.5)");
    EXPECT_EQ(query(to, "select (label is null)::text from share_links"), "true");
    EXPECT_EQ(query(to, "select count(*) from sessions"), "1");
    EXPECT_EQ(query(to, "select count(*) from result_cache"), "0");
    EXPECT_EQ(query(to, "select string_agg(name, ',' order by name) from schema_migrations"),
              query(from, "select string_agg(name, ',' order by name) from schema_migrations"));

    const BackupFiles target = files("target");
    EXPECT_EQ(readFile(target.modulesRoot / "fpv.osd" / "lib" / "osd.so"), std::string("\0\x01\x02 lib", 8));
    EXPECT_EQ(readFile(target.trustStore), R"({"keys": []})");
    EXPECT_EQ(filesUnder(dir_ / "target"), 2u) << "no staged file is left next to the restored ones";
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
    EXPECT_EQ(s.newerMigrations, std::vector<std::string>{"999_test.sql"});
    EXPECT_EQ(query(to, "select note from missions where id = 'm2'"), "from Second");
}

TEST_F(BackupTest, RefusesADatabaseThatHasASchema) {
    const std::string from = source();
    const std::string text = backup(from, files("source"));
    EXPECT_THROW((void)restore(from, text, RFMAP_SQL_DIR, "source"), BackupError);
    EXPECT_EQ(query(from, "select count(*) from missions"), "2");
}

TEST_F(BackupTest, RefusesABackupFromANewerServerAndChangesNothing) {
    const std::string newer = freshDatabase("rfmap_backup_newer");
    Database(newer, 1).migrate(migrationsWith("create table later (id int);"));
    const std::string text = backup(newer, files("newer"));
    const std::string to = freshDatabase("rfmap_backup_target");
    try {
        (void)restore(to, text);
        FAIL() << "restored a backup of an unknown migration";
    } catch (const BackupError& e) {
        EXPECT_NE(std::string(e.what()).find("999_test.sql"), std::string::npos) << e.what();
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

TEST_F(BackupTest, CommandLineBacksUpAndRestores) {
    const std::string from = source();
    const BackupFiles f = files("source");
    Config::Environment env{{"RFMAP_DATABASE_URL", from},
                            {"RFMAP__MODULES__ROOT", f.modulesRoot.string()},
                            {"RFMAP__MODULES__TRUSTSTORE", f.trustStore.string()}};
    std::istringstream none;
    std::ostringstream out;
    std::ostringstream err;
    const auto backupCmd = CommandLine::parse({"--backup"}, env, RFMAP_SQL_DIR);
    ASSERT_EQ(ServiceCommand::run(backupCmd, env, none, out, err), 0) << err.str();
    EXPECT_NE(err.str().find("backed up 8 tables, 5 rows and 2 files"), std::string::npos) << err.str();

    const BackupFiles t = files("target");
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

TEST_F(BackupTest, RefusesABackupWhoseTotalsDoNotMatch) {
    std::string text = backup(source(), files("source"));
    const auto at = text.find("\"rows\":5");
    ASSERT_NE(at, std::string::npos);
    text.replace(at, std::string("\"rows\":5").size(), "\"rows\":6");
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

TEST_F(BackupTest, AMissingMigrationFileAppliesNothing) {
    const fs::path sql = migrationsWith("create table later (id int);");
    const Migrations migrations(sql);
    fs::remove(sql / "999_test.sql");
    const std::string url = freshDatabase("rfmap_backup_target");
    pqxx::connection c(url);
    pqxx::work w(c);
    Migrations::Names done = Migrations::lock(w);
    EXPECT_THROW((void)migrations.apply(w, done), fs::filesystem_error);
    EXPECT_FALSE(done.contains("999_test.sql"));
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
    const BackupFiles target = files("target");
    fs::create_directories(target.trustStore / "in-the-way");
    const std::string to = freshDatabase("rfmap_backup_target");
    try {
        (void)restore(to, text);
        FAIL() << "moved the trust store over a directory";
    } catch (const BackupError& e) {
        EXPECT_NE(std::string(e.what()).find("into place by hand"), std::string::npos) << e.what();
    }
    EXPECT_EQ(query(to, "select count(*) from missions"), "2") << "the database committed before the move";
    EXPECT_EQ(readFile(target.trustStore.string() + ".rfmap-restoring"), R"({"keys": []})");
}

TEST_F(BackupTest, WaitsForAModuleChangeToFinish) {
    const std::string from = source();
    const BackupFiles f = files("source");
    ModuleGate gate;
    gate.attach(f.modulesRoot);
    std::unique_lock change(gate);
    auto pending = std::async(std::launch::async, [&] { return backup(from, f); });
    EXPECT_EQ(pending.wait_for(std::chrono::milliseconds(300)), std::future_status::timeout);
    change.unlock();
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
