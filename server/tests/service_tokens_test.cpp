#include "config.hpp"
#include "service_tokens.hpp"

#include <gtest/gtest.h>

using namespace rfmap;

namespace {

std::string digestOf(std::string_view token) { return Secrets::hex(Secrets::sha256(token)); }

json withTokens(const json& tokens) { return {{"services", {{"tokens", tokens}}}}; }

}  // namespace

TEST(ServiceTokensConfig, NoneUnlessConfigured) {
    EXPECT_TRUE(Config::fromJson(json::object()).serviceTokens.empty());
    EXPECT_TRUE(Config::fromJson({{"services", json::object()}}).serviceTokens.empty());
    EXPECT_EQ(Config::fromJson(json::object()).rateLimits.at("service").perMinute, 300.0);
}

TEST(ServiceTokensConfig, EntriesAreReadWithTheirDigestInLowercase) {
    const std::string upper = "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD";
    const Config c = Config::fromJson(withTokens({{{"name", "nightly-ci"}, {"sha256", upper}},
                                                  {{"name", "grafana.v2"}, {"sha256", digestOf("x")}}}));
    ASSERT_EQ(c.serviceTokens.size(), 2U);
    EXPECT_EQ(c.serviceTokens[0].name, "nightly-ci");
    EXPECT_EQ(c.serviceTokens[0].sha256, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // The admin page sees which services exist, not their digests.
    EXPECT_EQ(c.raw["services"]["tokens"],
              json::array({{{"name", "nightly-ci"}, {"sha256", "(set)"}}, {{"name", "grafana.v2"}, {"sha256", "(set)"}}}));
}

TEST(ServiceTokensConfig, TheEnvironmentCanSetThem) {
    const Config c = Config::load(
        {}, {{"RFMAP__SERVICES__TOKENS", json::array({{{"name", "ci"}, {"sha256", digestOf("t")}}}).dump()}});
    ASSERT_EQ(c.serviceTokens.size(), 1U);
    EXPECT_EQ(c.serviceTokens[0].name, "ci");
}

TEST(ServiceTokensConfig, BadEntriesStopTheServerFromStarting) {
    const std::string ok = digestOf("t");
    for (const json& bad : {json("ci"), json::array({"ci"}), json::array({{{"name", "ci"}}}),
                            json::array({{{"name", "CI"}, {"sha256", ok}}}),
                            json::array({{{"name", "-ci"}, {"sha256", ok}}}),
                            json::array({{{"name", "ci ci"}, {"sha256", ok}}}),
                            json::array({{{"name", std::string(65, 'a')}, {"sha256", ok}}}),
                            json::array({{{"name", "ci"}, {"sha256", "rfmap_svc_the-token-itself"}}}),
                            json::array({{{"name", "ci"}, {"sha256", ok.substr(1)}}}),
                            json::array({{{"name", "ci"}, {"sha256", ok}}, {{"name", "ci"}, {"sha256", ok}}})}) {
        EXPECT_THROW(Config::fromJson(withTokens(bad)), std::invalid_argument) << bad.dump();
    }
}

TEST(ServiceTokens, MatchNamesTheTokenPresented) {
    const Config c = Config::fromJson(
        withTokens({{{"name", "ci"}, {"sha256", digestOf("rfmap_svc_one")}}, {{"name", "grafana"}, {"sha256", digestOf("rfmap_svc_two")}}}));
    const ServiceTokens tokens = ServiceTokens::fromConfig(c.serviceTokens);
    EXPECT_FALSE(tokens.empty());
    EXPECT_EQ(tokens.match("rfmap_svc_one"), "ci");
    EXPECT_EQ(tokens.match("rfmap_svc_two"), "grafana");
    EXPECT_FALSE(tokens.match("rfmap_svc_three").has_value());
    EXPECT_FALSE(tokens.match("").has_value());
    EXPECT_FALSE(ServiceTokens({}).match("rfmap_svc_one").has_value());
    EXPECT_EQ(ServiceTokens::principalId("ci"), "service:ci");
}
