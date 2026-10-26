#include "ops_server.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <memory>
#include <stdexcept>
#include <string>

using namespace rfmap;

namespace {

Config::Ops localOps() {
    Config::Ops ops;
    ops.listen = "127.0.0.1";
    return ops;
}

/// An OpsServer on an ephemeral port and a client for it.
class OpsFixture {
public:
    OpsFixture(MetricsAccess access, bool metricsEnabled)
        : server(localOps(), std::move(access), metricsEnabled, log), port_(server.bindAny()),
          client_("127.0.0.1", port_) {
        server.setMetricsSource([] { return std::string("rfmap_up 1\n"); });
    }
    httplib::Result get(const std::string& path, const std::string& bearer = "") {
        httplib::Headers headers;
        if (!bearer.empty()) headers.emplace("Authorization", "Bearer " + bearer);
        return client_.Get(path, headers);
    }

    Logger log;
    OpsServer server;

private:
    int port_;
    httplib::Client client_;
};

MetricsAccess openToLoopback() { return {"", CidrList::parse({})}; }

/// A loopback port held by a plain socket (without SO_REUSEPORT, so a listener cannot share it)
/// until released; afterwards nothing listens on it.
class HeldPort {
public:
    HeldPort() : fd_(::socket(AF_INET, SOCK_STREAM, 0)) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof addr;
        const bool bound = fd_ >= 0 && ::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), len) == 0 &&  // NOSONAR
                           ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0;             // NOSONAR
        port_ = bound ? ntohs(addr.sin_port) : 0;
    }
    HeldPort(const HeldPort&) = delete;
    HeldPort& operator=(const HeldPort&) = delete;
    ~HeldPort() { release(); }

    [[nodiscard]] int port() const { return port_; }
    void release() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_;
    int port_ = 0;
};

}  // namespace

TEST(CidrListTest, MatchesNetworksAndSingleAddresses) {
    const CidrList list = CidrList::parse({"10.0.0.0/8", "192.168.1.7", "fd00::/8", "2001:db8::1"});
    EXPECT_TRUE(list.contains("10.255.3.4"));
    EXPECT_FALSE(list.contains("11.0.0.1"));
    EXPECT_TRUE(list.contains("192.168.1.7"));
    EXPECT_FALSE(list.contains("192.168.1.8"));
    EXPECT_TRUE(list.contains("fd12::1"));
    EXPECT_FALSE(list.contains("fe80::1"));
    EXPECT_TRUE(list.contains("2001:db8::1"));
    EXPECT_FALSE(list.contains("2001:db8::2"));
}

TEST(CidrListTest, MappedIpv4MatchesIpv4Networks) {
    const CidrList list = CidrList::parse({"10.0.0.0/8"});
    EXPECT_TRUE(list.contains("::ffff:10.1.2.3"));
    EXPECT_FALSE(list.contains("::ffff:11.1.2.3"));
    EXPECT_TRUE(CidrList::isLoopback("127.0.0.1"));
    EXPECT_TRUE(CidrList::isLoopback("127.5.5.5"));
    EXPECT_TRUE(CidrList::isLoopback("::1"));
    EXPECT_TRUE(CidrList::isLoopback("::ffff:127.0.0.1"));
    EXPECT_FALSE(CidrList::isLoopback("10.0.0.1"));
    EXPECT_FALSE(CidrList::isLoopback("not an address"));
}

TEST(CidrListTest, PrefixEdgesAndZeroLengthPrefix) {
    EXPECT_TRUE(CidrList::parse({"0.0.0.0/0"}).contains("203.0.113.9"));
    EXPECT_FALSE(CidrList::parse({"0.0.0.0/0"}).contains("2001:db8::1"));
    EXPECT_TRUE(CidrList::parse({"::/0"}).contains("2001:db8::1"));
    EXPECT_TRUE(CidrList::parse({"10.1.2.3/32"}).contains("10.1.2.3"));
    EXPECT_FALSE(CidrList::parse({"10.1.2.3/32"}).contains("10.1.2.4"));
    EXPECT_TRUE(CidrList::parse({"10.1.2.0/23"}).contains("10.1.3.255"));
    EXPECT_FALSE(CidrList::parse({"10.1.2.0/23"}).contains("10.1.4.0"));
}

TEST(CidrListTest, RejectsWhatIsNotANetwork) {
    EXPECT_THROW(CidrList::parse({"10.0.0.0/33"}), std::invalid_argument);
    EXPECT_THROW(CidrList::parse({"fd00::/129"}), std::invalid_argument);
    EXPECT_THROW(CidrList::parse({"10.0.0/8"}), std::invalid_argument);
    EXPECT_THROW(CidrList::parse({"10.0.0.0/x"}), std::invalid_argument);
    EXPECT_THROW(CidrList::parse({"10.0.0.0/-1"}), std::invalid_argument);
    EXPECT_THROW(CidrList::parse({"example.org"}), std::invalid_argument);
    EXPECT_THROW(CidrList::parse({""}), std::invalid_argument);
}

TEST(MetricsAccessTest, WithoutTokenOrNetworksOnlyLoopbackReads) {
    const MetricsAccess access = openToLoopback();
    EXPECT_EQ(access.decide("127.0.0.1", ""), MetricsAccess::Verdict::Allowed);
    EXPECT_EQ(access.decide("::1", ""), MetricsAccess::Verdict::Allowed);
    EXPECT_EQ(access.decide("10.0.0.5", ""), MetricsAccess::Verdict::Forbidden);
    EXPECT_EQ(access.decide("10.0.0.5", "anything"), MetricsAccess::Verdict::Forbidden);
}

TEST(MetricsAccessTest, TokenAloneAdmitsAnyAddressWithTheToken) {
    const MetricsAccess access("s3cret-metrics-token", CidrList::parse({}));
    EXPECT_EQ(access.decide("10.0.0.5", "s3cret-metrics-token"), MetricsAccess::Verdict::Allowed);
    EXPECT_EQ(access.decide("10.0.0.5", ""), MetricsAccess::Verdict::TokenRequired);
    EXPECT_EQ(access.decide("10.0.0.5", "wrong"), MetricsAccess::Verdict::Forbidden);
    // Loopback is no longer a free pass once a token is configured.
    EXPECT_EQ(access.decide("127.0.0.1", ""), MetricsAccess::Verdict::TokenRequired);
}

TEST(MetricsAccessTest, NetworksAloneAdmitByAddress) {
    const MetricsAccess access("", CidrList::parse({"10.42.0.0/16"}));
    EXPECT_EQ(access.decide("10.42.7.7", ""), MetricsAccess::Verdict::Allowed);
    EXPECT_EQ(access.decide("10.43.0.1", ""), MetricsAccess::Verdict::Forbidden);
    EXPECT_EQ(access.decide("127.0.0.1", ""), MetricsAccess::Verdict::Forbidden);
}

TEST(MetricsAccessTest, TokenAndNetworksBothMustHold) {
    const MetricsAccess access("tok", CidrList::parse({"10.42.0.0/16"}));
    EXPECT_EQ(access.decide("10.42.7.7", "tok"), MetricsAccess::Verdict::Allowed);
    EXPECT_EQ(access.decide("10.43.0.1", "tok"), MetricsAccess::Verdict::Forbidden);
    EXPECT_EQ(access.decide("10.42.7.7", ""), MetricsAccess::Verdict::TokenRequired);
    EXPECT_EQ(access.decide("10.42.7.7", "nope"), MetricsAccess::Verdict::Forbidden);
}

TEST(MetricsAccessTest, FromConfigAndDescription) {
    Config::Metrics m;
    m.token = "tok";
    m.allowFrom = {"10.0.0.0/8", "::1"};
    const MetricsAccess access = MetricsAccess::fromConfig(m);
    EXPECT_TRUE(access.hasToken());
    EXPECT_TRUE(access.hasNetworks());
    EXPECT_EQ(access.describe(), "bearer token and 2 allowed network(s)");
    EXPECT_EQ(openToLoopback().describe(),
              "loopback only (set metrics.token or metrics.allowFrom to scrape from elsewhere)");
    m.allowFrom = {"10.0.0.0/99"};
    EXPECT_THROW(MetricsAccess::fromConfig(m), std::invalid_argument);
}

TEST(OpsServerTest, HealthzAlwaysAnswersOk) {
    OpsFixture f(openToLoopback(), false);
    const auto res = f.get("/healthz");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(json::parse(res->body).at("status"), "ok");
    f.server.setDraining(true);
    EXPECT_EQ(f.get("/healthz")->status, 200);
}

TEST(OpsServerTest, ReadyzFollowsTheRequiredChecks) {
    OpsFixture f(openToLoopback(), false);
    bool databaseUp = true;
    f.server.readiness().addRequired("database", [&databaseUp] {
        return CheckResult{databaseUp, databaseUp ? "ok" : "database unreachable"};
    });
    f.server.readiness().addInformational("dem", [] { return CheckResult{false, "no DEM configured"}; });

    auto res = f.get("/readyz");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    json body = json::parse(res->body);
    EXPECT_TRUE(body.at("ready").get<bool>());
    EXPECT_TRUE(body.at("checks").at("database").at("ok").get<bool>());
    EXPECT_FALSE(body.at("checks").at("dem").at("ok").get<bool>());
    EXPECT_FALSE(body.at("checks").at("dem").at("required").get<bool>());

    databaseUp = false;
    res = f.get("/readyz");
    EXPECT_EQ(res->status, 503);
    body = json::parse(res->body);
    EXPECT_FALSE(body.at("ready").get<bool>());
    EXPECT_EQ(body.at("checks").at("database").at("detail"), "database unreachable");
}

TEST(OpsServerTest, ReadyzAnswers503WhileDraining) {
    OpsFixture f(openToLoopback(), false);
    f.server.setDraining(true);
    const auto res = f.get("/readyz");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 503);
    EXPECT_TRUE(json::parse(res->body).at("draining").get<bool>());
}

TEST(OpsServerTest, MetricsAreOffUnlessEnabled) {
    OpsFixture f(openToLoopback(), false);
    const auto res = f.get("/metrics");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST(OpsServerTest, MetricsFromLoopbackWithoutGuards) {
    OpsFixture f(openToLoopback(), true);
    const auto res = f.get("/metrics");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(res->body, "rfmap_up 1\n");
    EXPECT_TRUE(res->get_header_value("Content-Type").starts_with("text/plain"));
}

TEST(OpsServerTest, MetricsBehindTheirOwnToken) {
    OpsFixture f(MetricsAccess("metrics-token", CidrList::parse({})), true);
    auto res = f.get("/metrics");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
    EXPECT_EQ(res->get_header_value("WWW-Authenticate"), "Bearer");
    EXPECT_EQ(f.get("/metrics", "wrong")->status, 403);
    res = f.get("/metrics", "metrics-token");
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(res->body, "rfmap_up 1\n");
}

TEST(OpsServerTest, MetricsRefusedFromOutsideTheAllowedNetworks) {
    // The test client connects from loopback, which this list leaves out.
    OpsFixture f(MetricsAccess("", CidrList::parse({"10.0.0.0/8"})), true);
    const auto res = f.get("/metrics");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
}

TEST(OpsServerTest, ListenLogsABusyPortAndTakesAFreeOne) {
    HeldPort held;
    ASSERT_GT(held.port(), 0);
    Config::Ops ops = localOps();
    ops.port = held.port();
    Logger log;
    log.setQuiet(true);
    OpsServer blocked(ops, openToLoopback(), false, log);
    EXPECT_FALSE(blocked.listen());  // the port is taken; the error is logged, not thrown

    held.release();
    OpsServer server(ops, openToLoopback(), false, log);  // as a restarted process would
    ASSERT_TRUE(server.listen());
    httplib::Client client("127.0.0.1", ops.port);
    const auto live = client.Get("/healthz");
    ASSERT_TRUE(live);
    EXPECT_EQ(live->status, 200);
    server.stop();
}

TEST(OpsServerTest, AMetricsSourceThatThrowsIsA500WithoutDetails) {
    OpsFixture f(openToLoopback(), true);
    f.log.setQuiet(true);
    f.server.setMetricsSource([]() -> std::string { throw std::runtime_error("the counters are broken"); });
    const auto r = f.get("/metrics");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 500);
    EXPECT_EQ(r->body, R"({"message":"internal error"})");
}

TEST(OpsServerTest, UnknownPathIsJson404) {
    OpsFixture f(openToLoopback(), false);
    const auto res = f.get("/api/v1/health");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    EXPECT_EQ(json::parse(res->body).at("message"), "Not Found");
}
