#include "settings_sync.hpp"

#include "security.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <format>

namespace rfmap {

namespace {

/// One digest over the rows the database topics read: the built-in module switches and options,
/// and the settings table (cache limits). Any change to them, from any process or by hand,
/// changes it.
constexpr const char* kFingerprintSql =
    "select md5(coalesce((select string_agg(id || ':' || enabled::text || ':' || options::text, ';' order by id) "
    "from builtin_modules), '') || '|' || coalesce((select string_agg(key || '=' || value::text, ';' order by key) "
    "from settings), ''))";

constexpr std::chrono::milliseconds kAwait{1000};
constexpr std::chrono::milliseconds kReconnectWait{5000};

/// The host name (the pod name in a cluster) and a random part, so two processes on one host differ.
std::string newInstanceId() {
    std::array<char, 256> host{};
    std::string name = ::gethostname(host.data(), host.size() - 1) == 0 ? std::string(host.data()) : "rfmap";
    return name + "/" + Secrets::randomToken(9);
}

std::string isoTime(TimePoint t) { return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(t)); }

}  // namespace

std::optional<ChangeNotice> ChangeNotice::parse(std::string_view payload) {
    const json j = json::parse(payload, nullptr, false);
    if (!j.is_object() || !j.contains("topic") || !j.at("topic").is_string()) return std::nullopt;
    const json origin = j.value("origin", json());
    return ChangeNotice(j.at("topic").get<std::string>(), origin.is_string() ? origin.get<std::string>() : "");
}

std::string ChangeNotice::payload() const { return json{{"topic", topic_}, {"origin", origin_}}.dump(); }

SettingsSync::SettingsSync(Database& db, Options options, Logger& log)
    : db_(db), options_(std::move(options)), log_(log), instance_(newInstanceId()) {}

SettingsSync::~SettingsSync() { stop(); }

void SettingsSync::add(std::string topic, Source source, Reload reload) {
    topics_.emplace_back(std::move(topic), source, std::move(reload));
}

void SettingsSync::start() {
    if (options_.listen) {
        try {
            openListener();
        } catch (const pqxx::failure& e) {
            noteListener(false, e.what());
            log_.warn("settings", std::string("cannot listen for settings changes yet: ") + e.what());
        }
    }
    // From here on a listener that connects (again) reloads first: it may have missed notices.
    listenedBefore_ = true;
    reloadWhere(Pick::database(), "start");
    log_.info("settings", std::format("instance {}: {}, fingerprint poll every {} s", instance_,
                                      options_.listen ? "listening on " + std::string(ChangeNotice::kChannel)
                                                      : "not listening",
                                      options_.poll.count()));
    thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

void SettingsSync::stop() {
    if (!thread_.joinable()) return;
    thread_.request_stop();
    sleep_.notify_all();
    thread_.join();
    listener_.reset();
}

void SettingsSync::run(const std::stop_token& stop) {
    auto nextPoll = std::chrono::steady_clock::now() + options_.poll;
    while (!stop.stop_requested()) {
        if (options_.listen) listenOnce(stop);
        else sleepFor(stop, kAwait);
        if (std::chrono::steady_clock::now() < nextPoll || stop.stop_requested()) continue;
        pollNow();
        nextPoll = std::chrono::steady_clock::now() + options_.poll;
    }
}

void SettingsSync::listenOnce(const std::stop_token& stop) {
    try {
        if (!listener_) openListener();
        listener_->await_notification(std::chrono::duration_cast<std::chrono::seconds>(kAwait).count());
        if (!listener_->is_open()) lostListener("the connection closed", stop);
    } catch (const pqxx::failure& e) {
        lostListener(e.what(), stop);
    }
}

void SettingsSync::openListener() {
    auto c = std::make_unique<pqxx::connection>(options_.listenUrl);
    c->listen(ChangeNotice::kChannel, [this](const pqxx::notification& n) { onPayload(n.payload); });
    listener_ = std::move(c);
    noteListener(true, "");
    // Notices sent while the listener was down are lost: read everything again.
    if (listenedBefore_) {
        log_.info("settings", "listening again; reloading every topic");
        reloadWhere(Pick::all(), "listener reconnected");
    }
}

void SettingsSync::lostListener(const std::string& why, const std::stop_token& stop) {
    if (listener_) log_.warn("settings", "settings listener lost: " + why);
    listener_.reset();
    noteListener(false, why);
    sleepFor(stop, kReconnectWait);
}

void SettingsSync::sleepFor(const std::stop_token& stop, std::chrono::milliseconds wait) {
    std::unique_lock lock(sleepMutex_);
    sleep_.wait_for(lock, stop, wait, [] { return false; });
}

void SettingsSync::onPayload(std::string_view payload) {
    const auto notice = ChangeNotice::parse(payload);
    if (!notice) {
        log_.warn("settings", "ignored a message on " + std::string(ChangeNotice::kChannel) + " that is not a change notice");
        return;
    }
    handle(*notice);
}

void SettingsSync::handle(const ChangeNotice& notice) {
    if (notice.origin() == instance_) return;  // this process changed it and has the new value
    {
        std::lock_guard lock(stateMutex_);
        ++state_.notices;
    }
    const std::string reason = std::format("{} changed by {}", notice.topic(), notice.origin());
    const bool known = std::ranges::any_of(topics_, [&notice](const Topic& t) { return t.name == notice.topic(); });
    // A topic a newer server version knows and this one does not: reloading everything is safe.
    reloadWhere(notice.topic() == ChangeNotice::kEverything || !known ? Pick::all() : Pick::one(notice.topic()), reason);
}

void SettingsSync::pollNow() {
    const auto now = fingerprint();
    if (!now) return;
    {
        std::lock_guard lock(reloadMutex_);
        if (fingerprint_ == now) return;
    }
    log_.info("settings", "the settings rows changed without a notice; reloading");
    reloadWhere(Pick::database(), "fingerprint changed");
}

bool SettingsSync::publish(std::string_view topic) {
    const std::string payload = ChangeNotice(std::string(topic), instance_).payload();
    try {
        db_.tx([&payload](pqxx::work& w) {
            w.exec("select pg_notify($1, $2)", pqxx::params{ChangeNotice::kChannel, payload});
        });
        return true;
    } catch (const pqxx::failure& e) {
        log_.warn("settings", std::format("could not announce the change of {}: {}", topic, e.what()));
        return false;
    }
}

bool SettingsSync::reloadAll(bool everywhere) {
    reloadWhere(Pick::all(), everywhere ? "admin reset everywhere" : "admin reset here");
    return !everywhere || publish(ChangeNotice::kEverything);
}

bool SettingsSync::Pick::covers(const Topic& t) const {
    switch (kind) {
        case Kind::All: return true;
        case Kind::Database: return t.source == Source::Database;
        case Kind::One: return t.name == name;
    }
    return false;
}

void SettingsSync::reloadWhere(const Pick& which, std::string_view reason) {
    std::lock_guard lock(reloadMutex_);
    // Taken before the reads, so a change after this point shows at the next poll. Only a reload
    // of every database topic may move it, and only when all of them succeeded: otherwise the
    // poll would treat rows that no reload has read as up to date.
    std::optional<std::string> before;
    if (coversDatabase(which)) before = fingerprint();
    const std::uint64_t failed = runReloads(which);
    if (before && failed == 0) fingerprint_ = std::move(before);
    noteReload(failed, reason);
}

void SettingsSync::noteReload(std::uint64_t failed, std::string_view reason) {
    std::lock_guard lock(stateMutex_);
    ++state_.reloads;
    state_.failures += failed;
    state_.lastReloadAt = Clock::now();
    state_.lastReason = reason;
}

bool SettingsSync::coversDatabase(const Pick& which) const {
    return std::ranges::all_of(topics_, [&which](const Topic& t) { return t.source != Source::Database || which.covers(t); });
}

std::uint64_t SettingsSync::runReloads(const Pick& which) const {
    std::uint64_t failed = 0;
    for (const Topic& t : topics_) {
        if (!which.covers(t)) continue;
        try {
            t.reload();
        } catch (const std::exception& e) {  // NOSONAR(cpp:S1181): a reload of any kind failing must not end the thread
            ++failed;
            log_.error("settings", std::format("reloading {} failed: {}", t.name, e.what()));
        }
    }
    return failed;
}

std::optional<std::string> SettingsSync::fingerprint() const {
    try {
        return db_.tx([](pqxx::work& w) { return w.query_value<std::string>(kFingerprintSql); });
    } catch (const pqxx::failure& e) {
        log_.warn("settings", std::string("cannot read the settings fingerprint: ") + e.what());
        return std::nullopt;
    }
}

void SettingsSync::noteListener(bool listening, std::string_view error) {
    std::lock_guard lock(stateMutex_);
    state_.listening = listening;
    state_.listenError = error;
}

json SettingsSync::status() const {
    std::lock_guard lock(stateMutex_);
    json topics = json::array();
    for (const Topic& t : topics_) {
        topics.push_back({{"id", t.name}, {"source", t.source == Source::Database ? "database" : "files"}});
    }
    return {{"instance", instance_},
            {"channel", ChangeNotice::kChannel},
            {"listen",
             {{"enabled", options_.listen},
              {"connected", state_.listening},
              {"error", state_.listenError.empty() ? json() : json(state_.listenError)}}},
            {"pollSeconds", options_.poll.count()},
            {"topics", topics},
            {"notices", state_.notices},
            {"reloads", state_.reloads},
            {"failedReloads", state_.failures},
            {"lastReloadAt", state_.lastReloadAt ? json(isoTime(*state_.lastReloadAt)) : json()},
            {"lastReason", state_.lastReason}};
}

}  // namespace rfmap
