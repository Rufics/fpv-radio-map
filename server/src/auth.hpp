#pragma once

#include "common.hpp"
#include "security.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rfmap {

enum class Role { Edit, View };
std::string toString(Role r);
Role roleFromString(const std::string& s);  ///< throws HttpError 400

struct LinkRecord {
    std::string linkId;
    std::string missionId;
    Digest tokenHash{};
    std::string pinHash;
    Role role = Role::View;
    std::string label;
    TimePoint createdAt;
    std::optional<TimePoint> lastOpenedAt;
    bool revoked = false;
    int failedAttempts = 0;
    int lockouts = 0;
    std::optional<TimePoint> lockedUntil;
    int openCount = 0;                   ///< successful opens
    std::optional<TimePoint> expiresAt;  ///< none = until revoked
};

struct SessionRecord {
    Digest tokenHash{};
    std::string missionId;
    std::optional<std::string> linkId;
    Role role = Role::View;
    TimePoint expiresAt;
};

/// Outcome of a wrong PIN, decided atomically by the store so parallel guesses cannot each see
/// the same remaining count.
struct FailureOutcome {
    int remaining;
    std::optional<TimePoint> lockedUntil;
};

/// Persistence for credentials. PostgreSQL in the server, memory in unit tests.
class IAuthStore {
public:
    virtual ~IAuthStore() = default;
    [[nodiscard]] virtual std::optional<Digest> ownerKeyHash(const std::string& missionId) = 0;
    virtual void insertLink(const LinkRecord& link) = 0;
    [[nodiscard]] virtual std::vector<LinkRecord> links(const std::string& missionId) = 0;
    [[nodiscard]] virtual std::optional<LinkRecord> linkByToken(const Digest& tokenHash) = 0;
    /// Adds one failure; at `maxAttempts` locks until now + lockout·2^lockouts and resets the count.
    virtual FailureOutcome recordFailure(const std::string& linkId, int maxAttempts, std::chrono::seconds lockout,
                                         TimePoint now) = 0;
    virtual void recordSuccess(const std::string& linkId, TimePoint now) = 0;
    /// Marks the link revoked and deletes its sessions. False if the link is not the mission's.
    virtual bool revokeLink(const std::string& missionId, const std::string& linkId) = 0;
    virtual void insertSession(const SessionRecord& s) = 0;
    [[nodiscard]] virtual std::optional<SessionRecord> session(const Digest& tokenHash, TimePoint now) = 0;
    virtual void purgeExpired(TimePoint now) = 0;
};

class MemoryAuthStore final : public IAuthStore {
public:
    void addMission(const std::string& missionId, const Digest& ownerHash);

    std::optional<Digest> ownerKeyHash(const std::string& missionId) override;
    void insertLink(const LinkRecord& link) override;
    std::vector<LinkRecord> links(const std::string& missionId) override;
    std::optional<LinkRecord> linkByToken(const Digest& tokenHash) override;
    FailureOutcome recordFailure(const std::string& linkId, int maxAttempts, std::chrono::seconds lockout,
                                 TimePoint now) override;
    void recordSuccess(const std::string& linkId, TimePoint now) override;
    bool revokeLink(const std::string& missionId, const std::string& linkId) override;
    void insertSession(const SessionRecord& s) override;
    std::optional<SessionRecord> session(const Digest& tokenHash, TimePoint now) override;
    void purgeExpired(TimePoint now) override;

private:
    std::mutex mutex_;
    std::map<std::string, Digest> owners_;
    std::map<std::string, LinkRecord> links_;
    std::map<Digest, SessionRecord> sessions_;
};

/// Who is calling, once a session token checked out.
struct Principal {
    std::string missionId;
    Role role;
    std::optional<std::string> linkId;
};

/// Mission credentials: owner keys, share links with PINs, sessions.
class AuthService {
public:
    struct Settings {
        int maxAttempts = 5;
        std::chrono::seconds lockout{15 * 60};
        std::chrono::seconds sessionTtl{7 * 24 * 3600};
    };

    AuthService(IAuthStore& store, const PinHasher& pins, const IClock& clock, Settings settings);

    struct OwnerKey {
        std::string plain;  ///< returned to the creator once
        Digest hash;        ///< what is stored
    };
    [[nodiscard]] static OwnerKey newOwnerKey();
    /// Throws HttpError 401/403 unless `presented` is this mission's owner key.
    void requireOwner(const std::string& missionId, const std::string& presented);

    struct CreatedLink {
        LinkRecord record;
        std::string token;  ///< shown once
    };
    CreatedLink createLink(const std::string& missionId, const std::string& ownerKey, Role role, const std::string& pin,
                           const std::string& label, std::optional<TimePoint> expiresAt = std::nullopt);
    std::vector<LinkRecord> listLinks(const std::string& missionId, const std::string& ownerKey);
    void revokeLink(const std::string& missionId, const std::string& ownerKey, const std::string& linkId);

    struct Opened {
        std::string session;
        std::string missionId;
        Role role;
    };
    /// 401 wrong PIN (body: triesLeft), 404 unknown token, 410 revoked or expired, 429 paused (body: retryAfterS).
    Opened openLink(const std::string& token, const std::string& pin);
    /// The link behind a token, before the PIN: 404 unknown, 410 revoked or expired. Changes nothing.
    LinkRecord peekLink(const std::string& token);

    /// A session for a mission's creator (no link) or for a link holder.
    std::string createSession(const std::string& missionId, Role role, std::optional<std::string> linkId,
                              std::optional<TimePoint> notAfter = std::nullopt);
    /// Throws HttpError 401 for a missing, unknown or expired session.
    Principal authenticate(const std::string& bearer);

    [[nodiscard]] static json linkJson(const LinkRecord& l);

private:
    IAuthStore& store_;
    const PinHasher& pins_;
    const IClock& clock_;
    Settings settings_;
};

}  // namespace rfmap
