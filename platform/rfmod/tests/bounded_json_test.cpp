#include "rfmod/bounded_json.hpp"

#include <gtest/gtest.h>

#include <string>

using rfmod::BoundedJson;
using rfmod::JsonTooDeepError;

namespace {

std::string nestedArrays(std::size_t depth) { return std::string(depth, '[') + std::string(depth, ']'); }

}  // namespace

TEST(BoundedJson, ParsesOrdinaryDocuments) {
    const auto doc = BoundedJson::parse(R"({"a": [1, {"b": "x"}], "c": null})");
    EXPECT_EQ(doc.at("a").at(1).at("b"), "x");
    EXPECT_EQ(BoundedJson::parse(nestedArrays(BoundedJson::kMaxDepth)).dump().size(), 2 * BoundedJson::kMaxDepth);
}

TEST(BoundedJson, RefusesDeepNestingBeforeParsing) {
    EXPECT_THROW((void)BoundedJson::parse(nestedArrays(BoundedJson::kMaxDepth + 1)), JsonTooDeepError);
    // Deep enough to overflow the stack in a recursive dump: refused, not crashed.
    EXPECT_THROW((void)BoundedJson::parse(R"({"m": )" + nestedArrays(1'000'000) + "}"), JsonTooDeepError);
    EXPECT_THROW((void)BoundedJson::parse(std::string(2'000'000, '{')), JsonTooDeepError);
    EXPECT_FALSE(BoundedJson::withinDepth(R"({"a": {"b": {}}})", 2));
    EXPECT_TRUE(BoundedJson::withinDepth(R"({"a": {"b": {}}})", 3));
}

TEST(BoundedJson, BracketsInStringsDoNotCount) {
    const std::string text = R"({"s": "[[[[{{{{ \"[[[[ \\"})";
    EXPECT_TRUE(BoundedJson::withinDepth(text, 1));
    EXPECT_EQ(BoundedJson::parse(text, 1).at("s"), R"([[[[{{{{ "[[[[ \)");
}

TEST(BoundedJson, MalformedTextIsStillTheParsersError) {
    EXPECT_THROW((void)BoundedJson::parse("{\"a\": ]"), nlohmann::json::parse_error);
    EXPECT_THROW((void)BoundedJson::parse("]]]]"), nlohmann::json::parse_error);
}
