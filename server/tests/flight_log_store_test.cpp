// The flight log store over a real database: the per-mission limit counts only the flights a file
// adds, so importing the same file again never hits it. Linked into the integration binary, which
// ctest gives a throw-away database.

#include "flight_logs.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>

using namespace rfmap;

namespace {

/// A store on a fresh database with one mission in it.
class StoreRig {
public:
    explicit StoreRig(const std::string& url) : db_(prepare(url), Database::Settings{.poolSize = 1}), store_(db_) {
        rfmap::test::migrate(db_, RFMAP_SQL_DIR);
        db_.tx([](pqxx::work& w) {
            w.exec("insert into missions(id, owner_key_hash, name, doc) values ('m1', '\\x00', 'test', '{}')");
            return 0;
        });
    }

    [[nodiscard]] FlightLogStore& store() { return store_; }

    static std::vector<FlightLogStore::NewFlight> flights(int count) {
        std::vector<FlightLogStore::NewFlight> out;
        for (int i = 1; i <= count; ++i) out.push_back({i, std::nullopt, 10.0, json::object(), {{"index", i}}});
        return out;
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

    Database db_;
    FlightLogStore store_;
};

class FlightLogStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* url = std::getenv("RFMAP_TEST_DATABASE_URL");
        if (url == nullptr) GTEST_SKIP() << "RFMAP_TEST_DATABASE_URL not set (run through ctest)";
        rig_ = std::make_unique<StoreRig>(url);
    }

    std::unique_ptr<StoreRig> rig_;
};

}  // namespace

TEST_F(FlightLogStoreTest, TheLimitCountsOnlyTheFlightsAFileAdds) {
    FlightLogStore& store = rig_->store();
    const auto first = store.add("m1", "a.bbl", "sha-a", "fpv.blackbox", StoreRig::flights(2), 3);
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(first->size(), 2u);

    // The same file again is at the limit's edge but adds nothing: it answers the same ids.
    const auto again = store.add("m1", "copy.bbl", "sha-a", "fpv.blackbox", StoreRig::flights(2), 2);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(*again, *first);

    // A file that adds two more flights would make four, past three: nothing is stored.
    EXPECT_FALSE(store.add("m1", "b.bbl", "sha-b", "fpv.blackbox", StoreRig::flights(2), 3).has_value());
    EXPECT_EQ(store.list("m1").size(), 2u);
    EXPECT_TRUE(store.add("m1", "b.bbl", "sha-b", "fpv.blackbox", StoreRig::flights(1), 3).has_value());
    EXPECT_EQ(store.list("m1").size(), 3u);
}
