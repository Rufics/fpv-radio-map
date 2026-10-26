#pragma once

#include "common.hpp"
#include "database.hpp"
#include "logger.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace rfmap {

/// One message on the rfmap_changes channel: which settings changed, and which server process
/// changed them, so that process can skip its own notice.
class ChangeNotice {
public:
    static constexpr const char* kChannel = "rfmap_changes";
    /// The topic of the admin's reset: every process reloads everything it caches.
    static constexpr const char* kEverything = "all";

    ChangeNotice(std::string topic, std::string origin) : topic_(std::move(topic)), origin_(std::move(origin)) {}

    /// The notice in `payload`; nullopt for text that is not one (another program on the channel).
    [[nodiscard]] static std::optional<ChangeNotice> parse(std::string_view payload);
    [[nodiscard]] std::string payload() const;
    [[nodiscard]] const std::string& topic() const { return topic_; }
    [[nodiscard]] const std::string& origin() const { return origin_; }

private:
    std::string topic_;
    std::string origin_;
};

/// The topics the server keeps in step (SettingsSync), as they appear in change notices.
struct SettingsTopic {
    static constexpr const char* kBuiltins = "builtins";        ///< builtin_modules: switches and options
    static constexpr const char* kCacheLimits = "cache.limits";  ///< settings: the cache limits
    static constexpr const char* kModules = "modules";           ///< installed modules and their options (files)
};

/// Keeps this process's copies of the admin settings in step with the other server processes
/// on the same database. Each copy is a topic: a name and the reload that reads it again.
///
/// The settings are read once and kept in memory, so a request never queries them. A process
/// that writes one publishes a ChangeNotice (NOTIFY rfmap_changes) and every other process
/// reloads that topic within a moment, through one LISTEN connection of its own. Every
/// `pollSeconds` a fingerprint of the settings rows is compared as a backstop for a notice
/// that was lost, a listener that is down, or a row changed by hand in psql. The admin can
/// also reload on request, here or everywhere (POST /admin/settings/reload).
class SettingsSync {
public:
    struct Options {
        /// Where the listener connects; a direct connection, not a transaction-mode pooler.
        std::string listenUrl;
        bool listen = true;
        std::chrono::seconds poll{60};
    };
    /// Whether a topic is read from the database tables the fingerprint covers, or from files
    /// (module options), which only a notice or the admin's reset reloads.
    enum class Source { Database, Files };
    using Reload = std::function<void()>;

    SettingsSync(Database& db, Options options, Logger& log);
    ~SettingsSync();
    SettingsSync(const SettingsSync&) = delete;
    SettingsSync& operator=(const SettingsSync&) = delete;

    /// Registers a topic; all of them before start().
    void add(std::string topic, Source source, Reload reload);
    /// Starts listening and polling. Reloads the database topics once, so a change made between
    /// their first read and the LISTEN is not missed.
    void start();
    /// Stops the background thread; called by the destructor too.
    void stop();

    /// Tells the other processes that this one has changed `topic`; false (and logged) when the
    /// notice could not be sent. The poll then brings the others up to date on a database
    /// topic, but not on a files topic: the caller has to say so.
    bool publish(std::string_view topic);
    /// The admin's reset: reloads every topic here and, when `everywhere`, asks every other
    /// process to do the same; false when that notice could not be sent.
    bool reloadAll(bool everywhere);
    /// GET /admin/settings/sync: this process, its listener, its last reload.
    [[nodiscard]] json status() const;

    /// The id this process signs its notices with.
    [[nodiscard]] const std::string& instance() const { return instance_; }
    /// Acts on a notice as the listener does (also for tests).
    void handle(const ChangeNotice& notice);
    /// Compares the fingerprint now and reloads the database topics if it changed (also for tests).
    void pollNow();

private:
    struct Topic {
        std::string name;
        Source source;
        Reload reload;
    };
    /// What the status shows; guarded by stateMutex_.
    struct State {
        bool listening = false;
        std::string listenError;
        std::uint64_t notices = 0;
        std::uint64_t reloads = 0;
        std::uint64_t failures = 0;
        std::optional<TimePoint> lastReloadAt;
        std::string lastReason;
    };

    void run(const std::stop_token& stop);
    /// Waits up to a second for notices; reconnects a listener that is down.
    void listenOnce(const std::stop_token& stop);
    void openListener();
    void lostListener(const std::string& why, const std::stop_token& stop);
    void sleepFor(const std::stop_token& stop, std::chrono::milliseconds wait);
    void onPayload(std::string_view payload);

    /// Runs the reloads of the topics `which` selects, each on its own, and counts them.
    void reloadWhere(const std::function<bool(const Topic&)>& which, const std::string& reason);
    void reloadDatabase(const std::string& reason);
    /// Whether `which` selects every database topic: only such a reload may move the fingerprint.
    [[nodiscard]] bool coversDatabase(const std::function<bool(const Topic&)>& which) const;
    /// Runs the selected reloads; returns how many failed (logged).
    [[nodiscard]] std::uint64_t runReloads(const std::function<bool(const Topic&)>& which) const;
    /// A digest of the rows the database topics read; nullopt when the database cannot answer.
    [[nodiscard]] std::optional<std::string> fingerprint() const;
    void noteListener(bool listening, const std::string& error);

    Database& db_;
    Options options_;
    Logger& log_;
    std::string instance_;
    std::vector<Topic> topics_;
    std::mutex reloadMutex_;  ///< one reload at a time: the listener's, the poll's, the admin's
    std::optional<std::string> fingerprint_;  ///< guarded by reloadMutex_
    mutable std::mutex stateMutex_;
    State state_;
    std::unique_ptr<pqxx::connection> listener_;  ///< used by the background thread only
    bool listenedBefore_ = false;
    std::mutex sleepMutex_;
    std::condition_variable_any sleep_;
    std::jthread thread_;
};

}  // namespace rfmap
