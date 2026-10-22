// SchemaVersion: how two lists of migrations stand against each other.

#include "migrations.hpp"

#include <gtest/gtest.h>

using namespace rfmap;
using Relation = SchemaVersion::Relation;

namespace {

SchemaVersion version(std::initializer_list<MigrationId> ids) { return SchemaVersion(std::vector<MigrationId>(ids)); }

}  // namespace

TEST(SchemaVersionTest, ComparesByPosition) {
    const SchemaVersion two = version({{"001_a.sql", "x"}, {"002_b.sql", "y"}});
    const SchemaVersion one = version({{"001_a.sql", "x"}});
    EXPECT_EQ(two.compare(two).relation, Relation::Same);
    EXPECT_EQ(one.compare(two).relation, Relation::Behind);
    EXPECT_EQ(one.compare(two).at, 1u);
    EXPECT_EQ(two.compare(one).relation, Relation::Ahead);
    EXPECT_EQ(SchemaVersion().compare(one).relation, Relation::Behind);
    const auto other = version({{"001_a.sql", "x"}, {"002_c.sql", "y"}}).compare(two);
    EXPECT_EQ(other.relation, Relation::Diverged);
    EXPECT_EQ(other.at, 1u);
}

TEST(SchemaVersionTest, AChecksumDiffersOnlyWhenBothAreKnown) {
    const SchemaVersion known = version({{"001_a.sql", "x"}});
    EXPECT_EQ(version({{"001_a.sql", "z"}}).compare(known).relation, Relation::Diverged);
    EXPECT_EQ(version({{"001_a.sql", ""}}).compare(known).relation, Relation::Same);
}

TEST(SchemaVersionTest, ReadsBackWhatItWrites) {
    const SchemaVersion v = version({{"001_a.sql", "x"}, {"002_b", ""}});
    EXPECT_EQ(SchemaVersion::fromJson(v.toJson()).compare(v).relation, Relation::Same);
    EXPECT_EQ(SchemaVersion::fromJson(json::parse(R"([{"name":"001_a.sql"}])")).ids().front().checksum, "");
    EXPECT_THROW((void)SchemaVersion::fromJson(json::parse(R"(["001_a.sql"])")), MigrationError);
    EXPECT_THROW((void)SchemaVersion::fromJson(json::object()), MigrationError);
}

TEST(SchemaVersionTest, AStampNamesTheLastStepAndFingerprintsThemAll) {
    const SchemaVersion v = version({{"001_a.sql", "x"}, {"002_b", "y"}});
    const SchemaStamp stamp = v.stamp();
    EXPECT_EQ(stamp.version, "002_b");
    EXPECT_EQ(stamp.steps, 2u);
    EXPECT_EQ(stamp.fingerprint, v.fingerprint());
    EXPECT_NE(version({{"001_a.sql", "x"}, {"002_b", "z"}}).fingerprint(), stamp.fingerprint);
    EXPECT_EQ(SchemaStamp::fromJson(stamp.toJson()).fingerprint, stamp.fingerprint);
    EXPECT_THROW((void)version({{"001_a.sql", ""}}).stamp(), MigrationError);
    EXPECT_THROW((void)SchemaVersion().stamp(), MigrationError);
    EXPECT_THROW((void)SchemaStamp::fromJson(json::parse(R"({"version":"001_a.sql","steps":0,"fingerprint":"f"})")),
                 MigrationError);
}

TEST(SchemaVersionTest, TakesUnknownChecksumsFromTheSameStepOnly) {
    const SchemaVersion known = version({{"001_a.sql", "x"}, {"002_b", "y"}});
    const SchemaVersion filled = version({{"001_a.sql", ""}, {"002_c", ""}}).withChecksumsFrom(known);
    EXPECT_EQ(filled.ids()[0].checksum, "x");
    EXPECT_EQ(filled.ids()[1].checksum, "");
}
