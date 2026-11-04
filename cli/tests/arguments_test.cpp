#include "arguments.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using rfmap::cli::Arguments;
using rfmap::cli::UsageError;

namespace {

Arguments parse(std::vector<std::string> words) { return Arguments(words); }

}  // namespace

TEST(Arguments, SplitsPositionalWordsFlagsAndSwitches) {
    const Arguments a = parse({"--server", "http://x", "calc", "route", "m.json", "--summary", "--min-margin=6.5"});
    EXPECT_EQ(a.positional(), (std::vector<std::string>{"calc", "route", "m.json"}));
    EXPECT_EQ(a.get("--server"), "http://x");
    EXPECT_TRUE(a.has("--summary"));
    EXPECT_EQ(a.number("--min-margin"), 6.5);
    EXPECT_FALSE(a.get("--token").has_value());
}

TEST(Arguments, TheLastValueOfARepeatedFlagWins) {
    EXPECT_EQ(parse({"--server", "a", "--server", "b"}).get("--server"), "b");
}

TEST(Arguments, ShortHelpAndALoneDashArePositional) {
    const Arguments a = parse({"-h", "calc", "route", "-"});
    EXPECT_TRUE(a.has("--help"));
    EXPECT_EQ(a.word(2, "file"), "-");
}

TEST(Arguments, AFlagWithoutItsValueIsAUsageError) {
    EXPECT_THROW(parse({"health", "--server"}), UsageError);
}

TEST(Arguments, NeedAndWordNameWhatIsMissing) {
    const Arguments a = parse({"calc"});
    try {
        (void)a.need("--from");
        FAIL();
    } catch (const UsageError& e) {
        EXPECT_STREQ(e.what(), "--from is required");
    }
    EXPECT_THROW((void)a.word(1, "what to calculate"), UsageError);
}

TEST(Arguments, RequireOnlyRefusesUnknownFlagsAndExtraWords) {
    EXPECT_NO_THROW(parse({"calc", "route", "m.json", "--fail-on", "warn", "--token", "t"}).requireOnly({"--fail-on"}, 3));
    EXPECT_THROW(parse({"calc", "route", "m.json", "--min-margn", "3"}).requireOnly({"--min-margin"}, 3), UsageError);
    EXPECT_THROW(parse({"health", "extra"}).requireOnly({}, 1), UsageError);
}

TEST(Arguments, NumbersMustBeWholeAndFinite) {
    EXPECT_EQ(Arguments::parseNumber("-3.25", "--x"), -3.25);
    for (const char* bad : {"", "3dB", "nan", "inf", "1e999"}) {
        EXPECT_THROW((void)Arguments::parseNumber(bad, "--x"), UsageError) << bad;
    }
}

TEST(Arguments, CoordinatesNeedTheirPartsInsideTheWorld) {
    EXPECT_EQ(Arguments::coordinates("48.5,35.2,120", "--to", true), (std::vector<double>{48.5, 35.2, 120.0}));
    EXPECT_EQ(Arguments::coordinates("48.5,35.2", "--from", false), (std::vector<double>{48.5, 35.2}));
    EXPECT_THROW((void)Arguments::coordinates("48.5,35.2", "--to", true), UsageError);
    EXPECT_THROW((void)Arguments::coordinates("48.5,35.2,1", "--from", false), UsageError);
    EXPECT_THROW((void)Arguments::coordinates("91,35.2", "--from", false), UsageError);
    EXPECT_THROW((void)Arguments::coordinates("48.5,181", "--from", false), UsageError);
    EXPECT_THROW((void)Arguments::coordinates("48.5;35.2", "--from", false), UsageError);
}
