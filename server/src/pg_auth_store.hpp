#pragma once

#include "auth.hpp"
#include "database.hpp"

namespace rfmap {

/// Credentials in PostgreSQL (tables share_links, sessions, missions.owner_key_hash).
class PgAuthStore final : public IAuthStore {
public:
    explicit PgAuthStore(Database& db) : db_(db) {}

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
    Database& db_;
};

/// Seconds since the epoch, as PostgreSQL's to_timestamp() takes them.
double epochSeconds(TimePoint t);
TimePoint fromEpochSeconds(double s);
/// Hex digest back to bytes.
Digest digestFromHex(const std::string& hex);

}  // namespace rfmap
