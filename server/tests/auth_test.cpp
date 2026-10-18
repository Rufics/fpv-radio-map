#include "auth.hpp"
#include "rate_limiter.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <utility>

using namespace rfmap;
using rfmap::test::FakeClock;

namespace {

class AuthTest : public ::testing::Test {
protected:
    void SetUp() override {
        owner = AuthService::newOwnerKey();
        store.addMission("m1", owner.hash);
        store.addMission("m2", AuthService::newOwnerKey().hash);
    }

    /// Expects `f` to throw HttpError with `status`; returns its body.
    template <class F>
    json expectStatus(int status, F&& f) {
        try {
            std::forward<F>(f)();
        } catch (const HttpError& e) {
            EXPECT_EQ(e.status(), status) << e.what();
            return e.body();
        }
        ADD_FAILURE() << "expected HTTP " << status;
        return {};
    }

    FakeClock clock;
    MemoryAuthStore store;
    PinHasher pins{PinHasher::Cost::Interactive};
    AuthService auth{store, pins, clock, {5, std::chrono::minutes(15), std::chrono::hours(24)}};
    AuthService::OwnerKey owner;
};

}  // namespace

TEST_F(AuthTest, OwnerKeyIsRandomAndOnlyItsHashIsKept) {
    const auto a = AuthService::newOwnerKey();
    const auto b = AuthService::newOwnerKey();
    EXPECT_NE(a.plain, b.plain);
    EXPECT_GE(a.plain.size(), 40u);  // 256 bits in base64url plus prefix
    EXPECT_TRUE(Secrets::constantTimeEqual(a.hash, Secrets::sha256(a.plain)));
    EXPECT_EQ(a.plain.find(Secrets::hex(a.hash)), std::string::npos);
}

TEST_F(AuthTest, OwnerKeyGuardsLinkManagement) {
    expectStatus(401, [&] { (void)auth.listLinks("m1", ""); });
    expectStatus(403, [&] { (void)auth.listLinks("m1", "rfo_wrong"); });
    // The right key for another mission does not open this one.
    expectStatus(403, [&] { (void)auth.listLinks("m2", owner.plain); });
    expectStatus(403, [&] { (void)auth.listLinks("nope", owner.plain); });
    EXPECT_TRUE(auth.listLinks("m1", owner.plain).empty());

    const auto link = auth.createLink("m1", owner.plain, Role::View, "123456", "spotter");
    expectStatus(403, [&] { auth.revokeLink("m1", "rfo_wrong", link.record.linkId); });
    expectStatus(404, [&] { auth.revokeLink("m1", owner.plain, "unknown"); });
    const auto links = auth.listLinks("m1", owner.plain);
    ASSERT_EQ(links.size(), 1u);
    EXPECT_EQ(links[0].label, "spotter");
    // The PIN is stored as argon2id, never as given.
    EXPECT_EQ(links[0].pinHash.rfind("$argon2id$", 0), 0u);
    EXPECT_EQ(links[0].pinHash.find("123456"), std::string::npos);
}

TEST_F(AuthTest, PinMustBeSixDigits) {
    for (const char* bad : {"12345", "1234567", "abcdef", "12 456", ""}) {
        expectStatus(422, [&] { (void)auth.createLink("m1", owner.plain, Role::Edit, bad, ""); });
    }
}

TEST_F(AuthTest, OpenWithPinGivesASessionWithTheLinksRole) {
    const auto link = auth.createLink("m1", owner.plain, Role::View, "246810", "");
    const auto opened = auth.openLink(link.token, "246810");
    EXPECT_EQ(opened.missionId, "m1");
    EXPECT_EQ(opened.role, Role::View);
    const Principal p = auth.authenticate(opened.session);
    EXPECT_EQ(p.missionId, "m1");
    EXPECT_EQ(p.role, Role::View);
    EXPECT_EQ(p.linkId, link.record.linkId);
    expectStatus(404, [&] { (void)auth.openLink("rfl_unknown", "246810"); });
}

TEST_F(AuthTest, FiveWrongPinsPauseTheLinkWithRetryAfter) {
    const auto link = auth.createLink("m1", owner.plain, Role::Edit, "135790", "");
    for (int i = 0; i < 4; ++i) {
        const json b = expectStatus(401, [&] { (void)auth.openLink(link.token, "000000"); });
        EXPECT_EQ(b.at("triesLeft").get<int>(), 4 - i);
    }
    const json fifth = expectStatus(429, [&] { (void)auth.openLink(link.token, "000000"); });
    EXPECT_EQ(fifth.at("retryAfterS").get<long long>(), 15 * 60);
    // Paused: even the right PIN is refused, and the check happens before the PIN is hashed.
    const json paused = expectStatus(429, [&] { (void)auth.openLink(link.token, "135790"); });
    EXPECT_EQ(paused.at("retryAfterS").get<long long>(), 15 * 60);
    clock.advance(std::chrono::minutes(10));
    expectStatus(429, [&] { (void)auth.openLink(link.token, "135790"); });
    clock.advance(std::chrono::minutes(5));
    EXPECT_NO_THROW((void)auth.openLink(link.token, "135790"));
}

TEST_F(AuthTest, RepeatedLockoutsDoubleThePause) {
    const auto link = auth.createLink("m1", owner.plain, Role::Edit, "135790", "");
    for (int i = 0; i < 4; ++i) expectStatus(401, [&] { (void)auth.openLink(link.token, "111111"); });
    expectStatus(429, [&] { (void)auth.openLink(link.token, "111111"); });
    clock.advance(std::chrono::minutes(15));
    for (int i = 0; i < 4; ++i) expectStatus(401, [&] { (void)auth.openLink(link.token, "111111"); });
    const json second = expectStatus(429, [&] { (void)auth.openLink(link.token, "111111"); });
    EXPECT_EQ(second.at("retryAfterS").get<long long>(), 30 * 60);
}

TEST_F(AuthTest, RevokingALinkEndsItsSessionsAtOnce) {
    const auto link = auth.createLink("m1", owner.plain, Role::Edit, "112233", "");
    const auto opened = auth.openLink(link.token, "112233");
    const std::string creator = auth.createSession("m1", Role::Edit, std::nullopt);
    auth.revokeLink("m1", owner.plain, link.record.linkId);
    expectStatus(401, [&] { (void)auth.authenticate(opened.session); });
    expectStatus(410, [&] { (void)auth.openLink(link.token, "112233"); });
    // Other sessions of the mission are untouched.
    EXPECT_EQ(auth.authenticate(creator).missionId, "m1");
}

TEST_F(AuthTest, SessionsExpireAndUnknownOnesAreRefused) {
    const std::string s = auth.createSession("m1", Role::Edit, std::nullopt);
    EXPECT_EQ(s.rfind("rfs_", 0), 0u);
    EXPECT_NE(s, auth.createSession("m1", Role::Edit, std::nullopt));
    EXPECT_EQ(auth.authenticate(s).role, Role::Edit);
    expectStatus(401, [&] { (void)auth.authenticate(""); });
    expectStatus(401, [&] { (void)auth.authenticate("rfs_forged"); });
    clock.advance(std::chrono::hours(24));
    expectStatus(401, [&] { (void)auth.authenticate(s); });
}

TEST_F(AuthTest, PeekShowsTheLinkBeforeThePinAndChangesNothing) {
    const auto link = auth.createLink("m1", owner.plain, Role::View, "121212", "");
    EXPECT_EQ(auth.peekLink(link.token).role, Role::View);
    expectStatus(401, [&] { (void)auth.openLink(link.token, "000000"); });
    // Peeking neither counts as a try nor resets the count.
    for (int i = 0; i < 3; ++i) (void)auth.peekLink(link.token);
    const json b = expectStatus(401, [&] { (void)auth.openLink(link.token, "000000"); });
    EXPECT_EQ(b.at("triesLeft").get<int>(), 3);
    expectStatus(404, [&] { (void)auth.peekLink("rfl_unknown"); });
    auth.revokeLink("m1", owner.plain, link.record.linkId);
    expectStatus(410, [&] { (void)auth.peekLink(link.token); });
}

TEST_F(AuthTest, OpenCountAndExpiry) {
    const TimePoint until = clock.now() + std::chrono::hours(2);
    const auto link = auth.createLink("m1", owner.plain, Role::Edit, "454545", "", until);
    expectStatus(422, [&] { (void)auth.createLink("m1", owner.plain, Role::Edit, "454545", "", clock.now()); });
    const auto first = auth.openLink(link.token, "454545");
    (void)auth.openLink(link.token, "454545");
    const json listed = AuthService::linkJson(auth.listLinks("m1", owner.plain).front());
    EXPECT_EQ(listed.at("openCount").get<int>(), 2);
    EXPECT_EQ(listed.at("expiresAt").get<std::string>(), isoUtc(until));
    // The session never outlives the link, although the session TTL is 24 h.
    clock.advance(std::chrono::hours(2));
    expectStatus(401, [&] { (void)auth.authenticate(first.session); });
    expectStatus(410, [&] { (void)auth.openLink(link.token, "454545"); });
    expectStatus(410, [&] { (void)auth.peekLink(link.token); });
}

TEST(SecretsTest, ConstantTimeEqualComparesContent) {
    EXPECT_TRUE(Secrets::constantTimeEqual("admin-token", "admin-token"));
    EXPECT_FALSE(Secrets::constantTimeEqual("admin-token", "admin-tokeN"));
    EXPECT_FALSE(Secrets::constantTimeEqual("admin-token", "admin-token-longer"));
    EXPECT_FALSE(Secrets::constantTimeEqual("", "x"));
}

TEST(RateLimiterTest, BurstThenRefillAtTheConfiguredRate) {
    FakeClock clock;
    RateLimiter limiter({{"open", {6.0, 3.0}}}, clock);
    // Client addresses from the RFC 5737 documentation range.
    for (int i = 0; i < 3; ++i) EXPECT_NO_THROW(limiter.take("open", "192.0.2.1"));
    try {
        limiter.take("open", "192.0.2.1");
        FAIL() << "expected 429";
    } catch (const HttpError& e) {
        EXPECT_EQ(e.status(), 429);
        EXPECT_EQ(e.body().at("retryAfterS").get<long long>(), 10);  // 6 per minute
    }
    EXPECT_NO_THROW(limiter.take("open", "192.0.2.2"));  // per client
    clock.advance(std::chrono::seconds(10));
    EXPECT_NO_THROW(limiter.take("open", "192.0.2.1"));
}
