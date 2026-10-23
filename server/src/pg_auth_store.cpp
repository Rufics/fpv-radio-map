#include "pg_auth_store.hpp"

#include "service_errors.hpp"

#include <sodium.h>

namespace rfmap {

double epochSeconds(TimePoint t) {
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

TimePoint fromEpochSeconds(double s) {
    return TimePoint(std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s)));
}

Digest digestFromHex(const std::string& hex) {
    Digest d{};
    if (std::size_t len = 0;
        sodium_hex2bin(d.data(), d.size(), hex.c_str(), hex.size(), nullptr, &len, nullptr) != 0 || len != d.size()) {
        throw StoredDataError("bad digest in database");
    }
    return d;
}

namespace {

constexpr const char* kLinkColumns =
    "id, mission_id, encode(token_hash,'hex'), pin_hash, role, coalesce(label,''), extract(epoch from created_at)::float8, "
    "extract(epoch from last_opened_at)::float8, revoked_at is not null, failed_attempts, lockouts, "
    "extract(epoch from locked_until)::float8, open_count, extract(epoch from expires_at)::float8";

LinkRecord linkFrom(const pqxx::row& r) {
    LinkRecord l;
    l.linkId = r[0].as<std::string>();
    l.missionId = r[1].as<std::string>();
    l.tokenHash = digestFromHex(r[2].as<std::string>());
    l.pinHash = r[3].as<std::string>();
    l.role = roleFromString(r[4].as<std::string>());
    l.label = r[5].as<std::string>();
    l.createdAt = fromEpochSeconds(r[6].as<double>());
    if (!r[7].is_null()) l.lastOpenedAt = fromEpochSeconds(r[7].as<double>());
    l.revoked = r[8].as<bool>();
    l.failedAttempts = r[9].as<int>();
    l.lockouts = r[10].as<int>();
    if (!r[11].is_null()) l.lockedUntil = fromEpochSeconds(r[11].as<double>());
    l.openCount = r[12].as<int>();
    if (!r[13].is_null()) l.expiresAt = fromEpochSeconds(r[13].as<double>());
    return l;
}

}  // namespace

std::optional<Digest> PgAuthStore::ownerKeyHash(const std::string& missionId) {
    return db_.tx([&missionId](pqxx::work& w) -> std::optional<Digest> {
        const auto r = w.exec("select encode(owner_key_hash,'hex') from missions where id = $1", pqxx::params{missionId});
        if (r.empty()) return std::nullopt;
        return digestFromHex(r[0][0].as<std::string>());
    });
}

void PgAuthStore::insertLink(const LinkRecord& l) {
    db_.tx([&l](pqxx::work& w) {
        w.exec(
            "insert into share_links(id, mission_id, token_hash, pin_hash, role, label, created_at, expires_at) "
            "values ($1, $2, decode($3,'hex'), $4, $5, $6, to_timestamp($7), to_timestamp($8))",
            pqxx::params{l.linkId, l.missionId, Secrets::hex(l.tokenHash), l.pinHash, toString(l.role), l.label,
                         epochSeconds(l.createdAt),
                         l.expiresAt ? std::optional<double>(epochSeconds(*l.expiresAt)) : std::optional<double>()});
    });
}

std::vector<LinkRecord> PgAuthStore::links(const std::string& missionId) {
    return db_.tx([&missionId](pqxx::work& w) {
        std::vector<LinkRecord> out;
        for (const auto& r : w.exec(std::string("select ") + kLinkColumns +
                                        " from share_links where mission_id = $1 order by created_at",
                                    pqxx::params{missionId})) {
            out.push_back(linkFrom(r));
        }
        return out;
    });
}

std::optional<LinkRecord> PgAuthStore::linkByToken(const Digest& tokenHash) {
    return db_.tx([&tokenHash](pqxx::work& w) -> std::optional<LinkRecord> {
        const auto r = w.exec(std::string("select ") + kLinkColumns + " from share_links where token_hash = decode($1,'hex')",
                              pqxx::params{Secrets::hex(tokenHash)});
        if (r.empty()) return std::nullopt;
        return linkFrom(r[0]);
    });
}

namespace {

/// One statement under the row lock, so concurrent guesses are counted one by one. The CTE keeps
/// whether the link was paused before this attempt; the update reads the old row.
/// $1 link id, $2 max attempts, $3 lockout seconds, $4 now (epoch seconds).
constexpr const char* kBeginAttemptSql =
    "with before as (select id, coalesce(locked_until > to_timestamp($4), false) as paused "
    "  from share_links where id = $1 for update) "
    "update share_links s set "
    "  locked_until = case when before.paused then s.locked_until when s.failed_attempts + 1 >= $2 "
    "    then to_timestamp($4) + make_interval(secs => $3 * power(2, least(s.lockouts, 10))) else s.locked_until end, "
    "  lockouts = case when before.paused then s.lockouts when s.failed_attempts + 1 >= $2 then s.lockouts + 1 "
    "    else s.lockouts end, "
    "  failed_attempts = case when before.paused then s.failed_attempts when s.failed_attempts + 1 >= $2 then 0 "
    "    else s.failed_attempts + 1 end "
    "from before where s.id = before.id "
    "returning s.failed_attempts, before.paused, coalesce(s.locked_until > to_timestamp($4), false), "
    "  extract(epoch from s.locked_until)::float8";

}  // namespace

AttemptOutcome PgAuthStore::beginAttempt(const std::string& linkId, int maxAttempts, std::chrono::seconds lockout,
                                         TimePoint now) {
    return db_.tx([&linkId, maxAttempts, lockout, now](pqxx::work& w) {
        const auto r = w.exec(kBeginAttemptSql,
                              pqxx::params{linkId, maxAttempts, static_cast<double>(lockout.count()), epochSeconds(now)});
        if (r.empty()) throw StoredDataError("link vanished");
        const auto& row = r[0];
        const bool refused = row[1].as<bool>();
        if (const bool pausedNow = row[2].as<bool>(); !pausedNow) {
            return AttemptOutcome{false, maxAttempts - row[0].as<int>(), std::nullopt};
        }
        return AttemptOutcome{refused, 0, fromEpochSeconds(row[3].as<double>())};
    });
}

void PgAuthStore::recordSuccess(const std::string& linkId, TimePoint now) {
    db_.tx([&linkId, now](pqxx::work& w) {
        w.exec(
            "update share_links set failed_attempts = 0, lockouts = 0, locked_until = null, "
            "last_opened_at = to_timestamp($2), open_count = open_count + 1 where id = $1",
            pqxx::params{linkId, epochSeconds(now)});
    });
}

bool PgAuthStore::revokeLink(const std::string& missionId, const std::string& linkId) {
    return db_.tx([&missionId, &linkId](pqxx::work& w) {
        const auto r = w.exec(
            "update share_links set revoked_at = coalesce(revoked_at, now()) where id = $1 and mission_id = $2 returning id",
            pqxx::params{linkId, missionId});
        w.exec("delete from sessions where link_id = $1", pqxx::params{linkId});
        return !r.empty();
    });
}

void PgAuthStore::insertSession(const SessionRecord& s) {
    db_.tx([&s](pqxx::work& w) {
        w.exec(
            "insert into sessions(token_hash, mission_id, link_id, role, expires_at) "
            "values (decode($1,'hex'), $2, $3, $4, to_timestamp($5))",
            pqxx::params{Secrets::hex(s.tokenHash), s.missionId, s.linkId, toString(s.role), epochSeconds(s.expiresAt)});
    });
}

std::optional<SessionRecord> PgAuthStore::session(const Digest& tokenHash, TimePoint now) {
    return db_.tx([&tokenHash, now](pqxx::work& w) -> std::optional<SessionRecord> {
        // A session from a revoked link is gone already, but check the link too in case of a race.
        const auto r = w.exec(
            "select s.mission_id, s.link_id, s.role, extract(epoch from s.expires_at)::float8 from sessions s "
            "left join share_links l on l.id = s.link_id "
            "where s.token_hash = decode($1,'hex') and s.expires_at > to_timestamp($2) and l.revoked_at is null",
            pqxx::params{Secrets::hex(tokenHash), epochSeconds(now)});
        if (r.empty()) return std::nullopt;
        SessionRecord s;
        s.tokenHash = tokenHash;
        s.missionId = r[0][0].as<std::string>();
        if (!r[0][1].is_null()) s.linkId = r[0][1].as<std::string>();
        s.role = roleFromString(r[0][2].as<std::string>());
        s.expiresAt = fromEpochSeconds(r[0][3].as<double>());
        return s;
    });
}

void PgAuthStore::purgeExpired(TimePoint now) {
    db_.tx([now](pqxx::work& w) {
        w.exec("delete from sessions where expires_at <= to_timestamp($1)", pqxx::params{epochSeconds(now)});
    });
}

}  // namespace rfmap
