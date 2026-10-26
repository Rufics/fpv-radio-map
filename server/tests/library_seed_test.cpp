// Library seeding over a real database: every seed*.json file of a kind is imported once,
// tracked by its own name. Linked into the integration binary, which ctest gives a throw-away
// database.

#include "library.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <set>

using namespace rfmap;
namespace fs = std::filesystem;

namespace {

json antenna(const std::string& id) {
    return {{"id", id}, {"name", id}, {"verified", true}, {"type", "dipole"}, {"gainDbi", 2.15},
            {"polarization", "linear-vertical"}, {"bandHz", {4e8, 6e9}},
            {"source", {{"kind", "standard"}, {"reference", "half-wave dipole, 2.15 dBi"}}}};
}

void writeSeed(const fs::path& file, const std::vector<std::string>& ids) {
    json items = json::array();
    for (const auto& id : ids) items.push_back(antenna(id));
    std::ofstream(file) << items.dump();
}

/// A library on a fresh database and an empty seed directory of its own.
class SeedRig {
public:
    explicit SeedRig(const std::string& url)
        : dir_(rfmap::test::scratchDir("library-seed")), db_(prepare(url), 1), schemas_(RFMAP_SCHEMAS_DIR),
          library_(db_, schemas_) {
        rfmap::test::migrate(db_, RFMAP_SQL_DIR);
        fs::remove_all(dir_ / "antennas");
        fs::create_directories(dir_ / "antennas");
    }

    [[nodiscard]] Library& library() { return library_; }
    [[nodiscard]] const fs::path& dir() const { return dir_; }

    [[nodiscard]] std::set<std::string> antennaIds() const {
        std::set<std::string> ids;
        for (const json& item : library_.list("antennas")) ids.insert(item.at("id").get<std::string>());
        return ids;
    }

private:
    /// Empties the database (a rerun keeps the cluster) before the pool connects to it.
    static std::string prepare(const std::string& url) {
        pqxx::connection c(url);
        pqxx::work w(c);
        w.exec("drop schema public cascade; create schema public;");
        w.commit();
        return url;
    }

    fs::path dir_;
    Database db_;
    SchemaRegistry schemas_;
    Library library_;
};

class LibrarySeed : public ::testing::Test {
protected:
    void SetUp() override {
        const char* url = std::getenv("RFMAP_TEST_DATABASE_URL");
        if (url == nullptr) GTEST_SKIP() << "RFMAP_TEST_DATABASE_URL not set (run through ctest)";
        rig_ = std::make_unique<SeedRig>(url);
    }

    std::unique_ptr<SeedRig> rig_;
};

}  // namespace

TEST_F(LibrarySeed, ANewSeedFileReachesAServerThatImportedTheFirstOne) {
    writeSeed(rig_->dir() / "antennas" / "seed.json", {"a1", "a2"});
    const auto first = rig_->library().seed(rig_->dir());
    EXPECT_EQ(rig_->antennaIds(), (std::set<std::string>{"a1", "a2"}));
    EXPECT_TRUE(std::ranges::any_of(first, [](const std::string& l) { return l.starts_with("seeded 2 antennas"); }));

    // A later release edits the first file and ships a second one: only the new file is read.
    writeSeed(rig_->dir() / "antennas" / "seed.json", {"a1", "a2", "a3"});
    writeSeed(rig_->dir() / "antennas" / "seed-extra.json", {"b1"});
    std::ofstream(rig_->dir() / "antennas" / "notes.json") << "not a seed file";
    const auto second = rig_->library().seed(rig_->dir());
    EXPECT_EQ(rig_->antennaIds(), (std::set<std::string>{"a1", "a2", "b1"}));
    ASSERT_EQ(second.size(), 1u);
    EXPECT_TRUE(second.front().starts_with("seeded 1 antennas")) << second.front();
    EXPECT_NE(second.front().find("seed-extra.json"), std::string::npos) << second.front();

    // Nothing is imported twice.
    EXPECT_TRUE(rig_->library().seed(rig_->dir()).empty());
    for (const json& item : rig_->library().list("antennas")) EXPECT_FALSE(item.at("verified").get<bool>());
}
