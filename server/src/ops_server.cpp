#include "ops_server.hpp"

#include "security.hpp"

#include <httplib.h>

#include <arpa/inet.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <format>
#include <stdexcept>
#include <string_view>

namespace rfmap {

namespace {

constexpr int kOpsThreads = 2;

/// The token of an `Authorization: Bearer <token>` header. Per RFC 9110 the scheme name is
/// case-insensitive and one or more spaces may separate it from the token.
std::string bearerOf(const httplib::Request& req) {
    if (!req.has_header("Authorization")) return {};
    const std::string h = req.get_header_value("Authorization");
    constexpr std::string_view scheme = "Bearer ";
    if (const auto sameLetter = [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); };
        h.size() <= scheme.size() ||
        !std::ranges::equal(scheme, std::string_view(h).substr(0, scheme.size()), sameLetter)) {
        return {};
    }
    const auto start = h.find_first_not_of(' ', scheme.size());
    return start == std::string::npos ? std::string{} : h.substr(start);
}

void sendJson(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

/// "a.b.c.d/24" → ("a.b.c.d", 24); a bare address gets no prefix.
std::pair<std::string_view, std::optional<int>> splitPrefix(std::string_view entry) {
    const auto slash = entry.find('/');
    if (slash == std::string_view::npos) return {entry, std::nullopt};
    int prefix = -1;
    const std::string_view digits = entry.substr(slash + 1);
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), prefix);
    if (ec != std::errc{} || end != digits.data() + digits.size()) return {entry, -1};
    return {entry.substr(0, slash), prefix};
}

}  // namespace

// ---- CidrList ----

std::optional<CidrList::Bytes> CidrList::toBytes(std::string_view address) {
    // inet_pton needs a terminated string; addresses are short.
    if (address.empty() || address.size() > 45) return std::nullopt;
    const std::string text(address);
    Bytes out{};
    if (inet_pton(AF_INET6, text.c_str(), out.data()) == 1) return out;
    std::array<std::byte, 4> v4{};
    if (inet_pton(AF_INET, text.c_str(), v4.data()) != 1) return std::nullopt;
    out[10] = std::byte{0xff};
    out[11] = std::byte{0xff};
    std::ranges::copy(v4, out.begin() + 12);
    return out;
}

CidrList CidrList::parse(const std::vector<std::string>& entries) {
    CidrList list;
    for (const std::string& entry : entries) {
        const auto [text, prefix] = splitPrefix(entry);
        const auto bytes = toBytes(text);
        const bool v4 = text.find(':') == std::string_view::npos;
        const int bits = v4 ? 32 : 128;
        const int wanted = prefix.value_or(bits);
        if (!bytes || wanted < 0 || wanted > bits) throw std::invalid_argument("not an address or network: " + entry);
        list.networks_.push_back({*bytes, v4 ? wanted + 96 : wanted});
    }
    return list;
}

bool CidrList::matches(const Network& net, const Bytes& addr) {
    int bits = net.prefix;
    for (std::size_t i = 0; i < addr.size() && bits > 0; ++i, bits -= 8) {
        const int take = std::min(bits, 8);
        const auto mask = static_cast<std::byte>(0xffU << (8 - take));
        if ((net.address[i] & mask) != (addr[i] & mask)) return false;
    }
    return true;
}

bool CidrList::contains(std::string_view address) const {
    const auto bytes = toBytes(address);
    if (!bytes) return false;
    return std::ranges::any_of(networks_, [&bytes](const Network& n) { return matches(n, *bytes); });
}

bool CidrList::isLoopback(std::string_view address) {
    static const CidrList loopback = parse({"127.0.0.0/8", "::1/128"});
    return loopback.contains(address);
}

// ---- MetricsAccess ----

MetricsAccess MetricsAccess::fromConfig(const Config::Metrics& metrics) {
    return {metrics.token, CidrList::parse(metrics.allowFrom)};
}

MetricsAccess::Verdict MetricsAccess::decide(std::string_view peer, std::string_view bearer) const {
    using enum Verdict;
    if (hasToken()) {
        if (bearer.empty()) return TokenRequired;
        if (!Secrets::constantTimeEqual(bearer, token_)) return Forbidden;
    }
    if (hasNetworks()) return allowFrom_.contains(peer) ? Allowed : Forbidden;
    if (hasToken()) return Allowed;
    return CidrList::isLoopback(peer) ? Allowed : Forbidden;
}

std::string MetricsAccess::describe() const {
    if (hasToken() && hasNetworks()) return std::format("bearer token and {} allowed network(s)", allowFrom_.size());
    if (hasToken()) return "bearer token";
    if (hasNetworks()) return std::format("{} allowed network(s)", allowFrom_.size());
    return "loopback only (set metrics.token or metrics.allowFrom to scrape from elsewhere)";
}

// ---- OpsServer ----

OpsServer::OpsServer(const Config::Ops& ops, MetricsAccess access, bool metricsEnabled, Logger& log)
    : ops_(ops), access_(std::move(access)), metricsEnabled_(metricsEnabled), log_(log),
      http_(std::make_unique<httplib::Server>()) {
    routes();
}

OpsServer::~OpsServer() { stop(); }

void OpsServer::routes() {
    httplib::Server& s = *http_;
    // httplib takes ownership of the queue it gets and deletes it when the server stops.
    s.new_task_queue = [] { return std::make_unique<httplib::ThreadPool>(kOpsThreads).release(); };
    s.set_read_timeout(5, 0);
    s.set_write_timeout(5, 0);
    s.set_payload_max_length(1024);
    s.set_keep_alive_max_count(1);
    s.Get("/healthz", [this](const httplib::Request& req, httplib::Response& res) { getHealthz(req, res); });
    s.Get("/readyz", [this](const httplib::Request& req, httplib::Response& res) { getReadyz(req, res); });
    s.Get("/metrics", [this](const httplib::Request& req, httplib::Response& res) { getMetrics(req, res); });
    s.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (res.body.empty()) sendJson(res, res.status, {{"message", httplib::status_message(res.status)}});
    });
    s.set_exception_handler([this](const httplib::Request& req, httplib::Response& res, const std::exception_ptr&) {
        log_.error("ops", std::format("{} {}: unexpected exception", req.method, req.path));
        sendJson(res, 500, {{"message", "internal error"}});
    });
}

void OpsServer::getHealthz(const httplib::Request&, httplib::Response& res) const {
    sendJson(res, 200, {{"status", "ok"}});
}

void OpsServer::getReadyz(const httplib::Request&, httplib::Response& res) const {
    ReadinessChecks::Report report;
    if (!draining()) report = readiness_.run();
    // Checked again after the probes: a shutdown that began while they ran must not get a 200.
    if (draining()) {
        sendJson(res, 503, {{"ready", false}, {"draining", true}});
        return;
    }
    sendJson(res, report.ready ? 200 : 503, report.body);
}

void OpsServer::getMetrics(const httplib::Request& req, httplib::Response& res) const {
    using enum MetricsAccess::Verdict;
    if (!metricsEnabled_ || !metrics_) {
        sendJson(res, 404, {{"message", "metrics are off; set metrics.enabled in the server config"}});
        return;
    }
    switch (access_.decide(req.remote_addr, bearerOf(req))) {
        case TokenRequired:
            res.set_header("WWW-Authenticate", "Bearer");
            sendJson(res, 401, {{"message", "metrics token required"}});
            return;
        case Forbidden: sendJson(res, 403, {{"message", "metrics are not readable from this address"}}); return;
        case Allowed: break;
    }
    res.status = 200;
    res.set_content(metrics_(), "text/plain; version=0.0.4; charset=utf-8");
}

bool OpsServer::listen() {
    if (!http_->bind_to_port(ops_.listen, ops_.port)) {
        log_.error("ops", std::format("could not listen on {}:{}", ops_.listen, ops_.port));
        return false;
    }
    log_.info("ops", std::format("health, readiness and metrics on {}:{}; metrics {}: {}", ops_.listen, ops_.port,
                                 metricsEnabled_ ? "on" : "off", access_.describe()));
    serveOnThread();
    return true;
}

int OpsServer::bindAny() {
    const int port = http_->bind_to_any_port(ops_.listen);
    if (port > 0) serveOnThread();
    return port;
}

void OpsServer::serveOnThread() {
    thread_ = std::jthread([this] { http_->listen_after_bind(); });
}

void OpsServer::stop() {
    if (http_) http_->stop();
    if (thread_.joinable()) thread_.join();
}

}  // namespace rfmap
