#include "rfmod/confined_root.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

namespace fs = std::filesystem;
using rfmod::ConfinedRoot;
using rfmod::PathEscapeError;

TEST(ConfinedRoot, CreatesAndCanonicalisesTheRoot) {
    const fs::path base = rfmod::test::scratchDir("confined-create");
    fs::remove_all(base / "root");
    const ConfinedRoot root(base / "sub" / ".." / "root");
    EXPECT_TRUE(fs::is_directory(base / "root"));
    EXPECT_EQ(root.path(), fs::canonical(base / "root"));
}

TEST(ConfinedRoot, ResolvesPathsInside) {
    const ConfinedRoot root(rfmod::test::scratchDir("confined-inside"));
    EXPECT_EQ(root.resolve("installed/a.b/lib/x.so"), root.path() / "installed/a.b/lib/x.so");
    EXPECT_EQ(root.confine(root.path() / "a" / ".." / "b"), root.path() / "b");
}

TEST(ConfinedRoot, RefusesPathsOutside) {
    const fs::path base = rfmod::test::scratchDir("confined-outside");
    const ConfinedRoot root(base / "root");
    EXPECT_THROW((void)root.resolve("../escape"), PathEscapeError);
    EXPECT_THROW((void)root.resolve("/etc/passwd"), PathEscapeError);
    EXPECT_THROW((void)root.confine(root.path()), PathEscapeError);
    // A sibling whose name merely starts with the root's name is still outside.
    EXPECT_THROW((void)root.confine(base / "root-other" / "x"), PathEscapeError);
}

TEST(ConfinedRoot, RefusesSymlinksLeadingOutside) {
    const fs::path base = rfmod::test::scratchDir("confined-symlink");
    const ConfinedRoot root(base / "root");
    fs::create_directories(base / "elsewhere");
    fs::remove(root.path() / "link");
    fs::create_directory_symlink(base / "elsewhere", root.path() / "link");
    EXPECT_THROW((void)root.resolve("link/lib.so"), PathEscapeError);
}
