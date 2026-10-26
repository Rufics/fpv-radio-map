#pragma once

// The operations listener: a second, small HTTP server for the things that watch this process
// rather than use it. It answers GET /healthz (the process is alive), GET /readyz (it can take
// traffic, see readiness.hpp) and GET /metrics (Prometheus text). It has its own threads, so a
// probe is answered while every request thread is busy, and it is never behind the rate limiter.
//
// Health answers to anyone who can reach the port: they say nothing but "up" or "not ready".
// Metrics describe the deployment, so they are guarded: a bearer token of their own, kept in a
// secret and never shared with the admin API, and/or a list of networks the scraper lives in.
// With neither configured, only the loopback interface may read them.

#include "config.hpp"
#include "logger.hpp"
#include "readiness.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace httplib {
class Server;
struct Request;
struct Response;
}  // namespace httplib

namespace rfmap {

/// Networks in CIDR notation ("10.0.0.0/8", "fd00::/8") or single addresses ("127.0.0.1").
/// IPv4 addresses, mapped or not, and IPv6 addresses are matched in one 128-bit space.
class CidrList {
public:
    /// Throws std::invalid_argument naming the entry that is not an address or a network.
    static CidrList parse(const std::vector<std::string>& entries);

    [[nodiscard]] bool contains(std::string_view address) const;
    [[nodiscard]] bool empty() const { return networks_.empty(); }
    [[nodiscard]] std::size_t size() const { return networks_.size(); }

    /// Whether `address` is a loopback address (127.0.0.0/8 or ::1), mapped or not.
    [[nodiscard]] static bool isLoopback(std::string_view address);

private:
    using Bytes = std::array<std::byte, 16>;
    struct Network {
        Bytes address{};
        int prefix = 0;  ///< bits of the 128-bit form
    };
    /// An address as 16 bytes; IPv4 becomes ::ffff:a.b.c.d. Nothing for text that is no address.
    static std::optional<Bytes> toBytes(std::string_view address);
    static bool matches(const Network& net, const Bytes& addr);

    std::vector<Network> networks_;
};

/// Who may read /metrics: the token, the networks, or loopback when neither is configured.
class MetricsAccess {
public:
    enum class Verdict { Allowed, TokenRequired, Forbidden };

    /// Throws std::invalid_argument for a network entry it cannot parse.
    static MetricsAccess fromConfig(const Config::Metrics& metrics);

    MetricsAccess(std::string token, CidrList allowFrom) : token_(std::move(token)), allowFrom_(std::move(allowFrom)) {}

    /// `peer` is the connection's remote address; `bearer` the credentials sent, or empty.
    [[nodiscard]] Verdict decide(std::string_view peer, std::string_view bearer) const;
    [[nodiscard]] bool hasToken() const { return !token_.empty(); }
    [[nodiscard]] bool hasNetworks() const { return !allowFrom_.empty(); }
    /// One line for the start-up log describing what guards the metrics.
    [[nodiscard]] std::string describe() const;

private:
    std::string token_;
    CidrList allowFrom_;
};

/// The listener. Construct, add checks, then `listen` (a port of its own) or `bindAny` (tests);
/// `stop` ends it. Every handler is small and touches no server state beyond what it is given.
class OpsServer {
public:
    using MetricsSource = std::function<std::string()>;

    OpsServer(const Config::Ops& ops, MetricsAccess access, bool metricsEnabled, Logger& log);
    ~OpsServer();
    OpsServer(const OpsServer&) = delete;
    OpsServer& operator=(const OpsServer&) = delete;
    OpsServer(OpsServer&&) = delete;
    OpsServer& operator=(OpsServer&&) = delete;

    [[nodiscard]] ReadinessChecks& readiness() { return readiness_; }
    void setMetricsSource(MetricsSource source) { metrics_ = std::move(source); }

    /// Binds the configured address and port and serves on a thread of its own. False if the
    /// port could not be bound; the error is logged.
    bool listen();
    /// Binds an ephemeral port on the configured address and serves; returns the port (tests).
    int bindAny();
    void stop();

    /// While draining, /readyz answers 503 so that a balancer stops sending new requests.
    void setDraining(bool draining) { draining_.store(draining); }
    [[nodiscard]] bool draining() const { return draining_.load(); }

private:
    void routes();
    void serveOnThread();
    void getHealthz(const httplib::Request& req, httplib::Response& res) const;
    void getReadyz(const httplib::Request& req, httplib::Response& res) const;
    void getMetrics(const httplib::Request& req, httplib::Response& res) const;

    Config::Ops ops_;
    MetricsAccess access_;
    bool metricsEnabled_;
    Logger& log_;
    ReadinessChecks readiness_;
    MetricsSource metrics_;
    std::atomic<bool> draining_{false};
    std::unique_ptr<httplib::Server> http_;
    std::jthread thread_;
};

}  // namespace rfmap
