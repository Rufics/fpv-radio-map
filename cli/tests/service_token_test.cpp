#include "service_token.hpp"

#include <gtest/gtest.h>

using rfmap::cli::ServiceToken;

TEST(ServiceToken, Base64UrlFollowsRfc4648WithoutPadding) {
    // RFC 4648 section 10, with the URL alphabet and no padding.
    EXPECT_EQ(ServiceToken::base64url(""), "");
    EXPECT_EQ(ServiceToken::base64url("f"), "Zg");
    EXPECT_EQ(ServiceToken::base64url("fo"), "Zm8");
    EXPECT_EQ(ServiceToken::base64url("foo"), "Zm9v");
    EXPECT_EQ(ServiceToken::base64url("foobar"), "Zm9vYmFy");
    EXPECT_EQ(ServiceToken::base64url("\xfb\xff"), "-_8");
}

TEST(ServiceToken, Sha256MatchesTheStandardVector) {
    EXPECT_EQ(ServiceToken::sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(ServiceToken, ANewTokenIsPrefixedRandomAndHashed) {
    const ServiceToken a = ServiceToken::generate();
    const ServiceToken b = ServiceToken::generate();
    EXPECT_TRUE(a.token.starts_with(ServiceToken::kPrefix));
    EXPECT_EQ(a.token.size(), ServiceToken::kPrefix.size() + 43);  // 32 bytes of base64url
    EXPECT_EQ(a.sha256, ServiceToken::sha256Hex(a.token));
    EXPECT_EQ(a.sha256.size(), 64U);
    EXPECT_NE(a.token, b.token);
}
