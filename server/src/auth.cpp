#include "auth.hpp"

#include <algorithm>

namespace rfmap {

std::string toString(Role r) { return r == Role::Edit ? "edit" : "view"; }

Role roleFromString(std::string_view s) {
    if (s == "edit") return Role::Edit;
    if (s == "view") return Role::View;
    throw HttpError(400, "role must be edit or view");
}

// ---- memory store ----

void MemoryAuthStore::addMission(const std::string& missionId, const Digest& ownerHash) {
    std::lock_guard lock(mutex_);
    owners_[missionId] = ownerHash;
}

std::optional<Digest> MemoryAuthStore::ownerKeyHash(const std::string& missionId) {
    std::lock_guard lock(mutex_);
    const auto it = owners_.find(missionId);
    if (it == owners_.end()) return std::nullopt;
    return it->second;
}

void MemoryAuthStore::insertLink(const LinkRecord& link) {
    std::lock_guard lock(mutex_);
    links_[link.linkId] = link;
}

std::vector<LinkRecord> MemoryAuthStore::links(const std::string& missionId) {
    std::lock_guard lock(mutex_);
    std::vector<LinkRecord> out;
    for (const auto& [id, l] : links_) {
        if (l.missionId == missionId) out.push_back(l);
    }
    return out;
}

std::optional<LinkRecord> MemoryAuthStore::linkByToken(const Digest& tokenHash) {
    std::lock_guard lock(mutex_);
    for (const auto& [id, l] : links_) {
        if (Secrets::constantTimeEqual(l.tokenHash, tokenHash)) return l;
    }
    return std::nullopt;
}

FailureOutcome MemoryAuthStore::recordFailure(const std::string& linkId, int maxAttempts, std::chrono::seconds lockout,
                                              TimePoint now) {
    std::lock_guard lock(mutex_);
    LinkRecord& l = links_.at(linkId);
    l.failedAttempts += 1;
    if (l.failedAttempts >= maxAttempts) {
        l.lockedUntil = now + lockout * (1 << std::min(l.lockouts, 10));
        l.lockouts += 1;
        l.failedAttempts = 0;
        return {0, l.lockedUntil};
    }
    return {maxAttempts - l.failedAttempts, std::nullopt};
}

void MemoryAuthStore::recordSuccess(const std::string& linkId, TimePoint now) {
    std::lock_guard lock(mutex_);
    LinkRecord& l = links_.at(linkId);
    l.failedAttempts = 0;
    l.lockouts = 0;
    l.lastOpenedAt = now;
    l.openCount += 1;
}

bool MemoryAuthStore::revokeLink(const std::string& missionId, const std::string& linkId) {
    std::lock_guard lock(mutex_);
    const auto it = links_.find(linkId);
    if (it == links_.end() || it->second.missionId != missionId) return false;
    it->second.revoked = true;
    for (auto s = sessions_.begin(); s != sessions_.end();) {
        s = s->second.linkId == linkId ? sessions_.erase(s) : std::next(s);
    }
    return true;
}

void MemoryAuthStore::insertSession(const SessionRecord& s) {
    std::lock_guard lock(mutex_);
    sessions_[s.tokenHash] = s;
}

std::optional<SessionRecord> MemoryAuthStore::session(const Digest& tokenHash, TimePoint now) {
    std::lock_guard lock(mutex_);
    const auto it = sessions_.find(tokenHash);
    if (it == sessions_.end() || it->second.expiresAt <= now) return std::nullopt;
    return it->second;
}

void MemoryAuthStore::purgeExpired(TimePoint now) {
    std::lock_guard lock(mutex_);
    for (auto s = sessions_.begin(); s != sessions_.end();) {
        s = s->second.expiresAt <= now ? sessions_.erase(s) : std::next(s);
    }
}

// ---- service ----

namespace {
/// Whole seconds until `until`, rounded up. Rounded to milliseconds first: the time comes back
/// from the database with microsecond precision, which must not turn 900 s into 901 s.
long long secondsUntil(TimePoint until, TimePoint now) {
    const auto ms = std::chrono::round<std::chrono::milliseconds>(until - now).count();
    return (ms + 999) / 1000;
}
}  // namespace

AuthService::AuthService(IAuthStore& store, const PinHasher& pins, PinGate& gate, const IClock& clock,
                         Settings settings)
    : store_(store), pins_(pins), gate_(gate), clock_(clock), settings_(settings) {}

AuthService::OwnerKey AuthService::newOwnerKey() {
    OwnerKey k;
    k.plain = "rfo_" + Secrets::randomToken(32);
    k.hash = Secrets::sha256(k.plain);
    return k;
}

void AuthService::requireOwner(const std::string& missionId, const std::string& presented) {
    if (presented.empty()) throw HttpError(401, "X-Owner-Key header required");
    const auto stored = store_.ownerKeyHash(missionId);
    // Hash the presented key even for an unknown mission, so timing says nothing about which exist.
    const Digest given = Secrets::sha256(presented);
    if (!stored || !Secrets::constantTimeEqual(*stored, given)) throw HttpError(403, "owner key does not match");
}

AuthService::CreatedLink AuthService::createLink(const std::string& missionId, const std::string& ownerKey, Role role,
                                                 std::string_view pin, std::string_view label,
                                                 std::optional<TimePoint> expiresAt) {
    requireOwner(missionId, ownerKey);
    if (pin.size() != 6 || !std::ranges::all_of(pin, [](char c) { return c >= '0' && c <= '9'; })) {
        throw HttpError(422, "pin must be exactly 6 digits");
    }
    if (label.size() > 120) throw HttpError(422, "label is longer than 120 characters");
    if (expiresAt && *expiresAt <= clock_.now()) throw HttpError(422, "expiresAt is in the past");
    CreatedLink out;
    out.token = "rfl_" + Secrets::randomToken(32);
    out.record.linkId = Secrets::randomToken(12);
    out.record.missionId = missionId;
    out.record.tokenHash = Secrets::sha256(out.token);
    out.record.pinHash = gate_.run([this, pin] { return pins_.hash(pin); });
    out.record.role = role;
    out.record.label = label;
    out.record.createdAt = clock_.now();
    out.record.expiresAt = expiresAt;
    store_.insertLink(out.record);
    return out;
}

std::vector<LinkRecord> AuthService::listLinks(const std::string& missionId, const std::string& ownerKey) {
    requireOwner(missionId, ownerKey);
    return store_.links(missionId);
}

void AuthService::revokeLink(const std::string& missionId, const std::string& ownerKey, const std::string& linkId) {
    requireOwner(missionId, ownerKey);
    if (!store_.revokeLink(missionId, linkId)) throw HttpError(404, "no such link on this mission");
}

LinkRecord AuthService::peekLink(const std::string& token) {
    const auto link = store_.linkByToken(Secrets::sha256(token));
    if (!link) throw HttpError(404, "unknown link");
    if (link->revoked) throw HttpError(410, "this link was revoked by the mission owner");
    if (link->expiresAt && *link->expiresAt <= clock_.now()) throw HttpError(410, "this link has expired");
    return *link;
}

AuthService::Opened AuthService::openLink(const std::string& token, const std::string& pin) {
    const std::optional<LinkRecord> link = peekLink(token);
    const TimePoint now = clock_.now();
    if (link->lockedUntil && *link->lockedUntil > now) {
        const auto s = secondsUntil(*link->lockedUntil, now);
        throw HttpError(429, "too many wrong PINs; the link is paused", {{"retryAfterS", s}});
    }
    // After the pause check, so a paused link costs no hashing slot.
    if (!gate_.run([this, &link, &pin] { return pins_.verify(link->pinHash, pin); })) {
        const FailureOutcome f = store_.recordFailure(link->linkId, settings_.maxAttempts, settings_.lockout, now);
        if (f.lockedUntil) {
            const auto s = secondsUntil(*f.lockedUntil, now);
            throw HttpError(429, "too many wrong PINs; the link is paused", {{"retryAfterS", s}});
        }
        throw HttpError(401, "wrong PIN", {{"triesLeft", f.remaining}});
    }
    store_.recordSuccess(link->linkId, now);
    return {createSession(link->missionId, link->role, link->linkId, link->expiresAt), link->missionId, link->role};
}

std::string AuthService::createSession(const std::string& missionId, Role role, std::optional<std::string> linkId,
                                       std::optional<TimePoint> notAfter) {
    const std::string token = "rfs_" + Secrets::randomToken(32);
    TimePoint expires = clock_.now() + settings_.sessionTtl;
    if (notAfter && *notAfter < expires) expires = *notAfter;  // a session never outlives its link
    store_.insertSession({Secrets::sha256(token), missionId, std::move(linkId), role, expires});
    return token;
}

Principal AuthService::authenticate(const std::string& bearer) {
    if (bearer.empty()) throw HttpError(401, "session required (Authorization: Bearer <session>)");
    const auto s = store_.session(Secrets::sha256(bearer), clock_.now());
    if (!s) throw HttpError(401, "session unknown or expired");
    return {s->missionId, s->role, s->linkId};
}

json AuthService::linkJson(const LinkRecord& l) {
    json j = {{"linkId", l.linkId}, {"role", toString(l.role)}, {"label", l.label},
              {"createdAt", isoUtc(l.createdAt)}, {"revoked", l.revoked}};
    if (l.lastOpenedAt) j["lastOpenedAt"] = isoUtc(*l.lastOpenedAt);
    if (l.lockedUntil) j["pausedUntil"] = isoUtc(*l.lockedUntil);
    j["openCount"] = l.openCount;
    if (l.expiresAt) j["expiresAt"] = isoUtc(*l.expiresAt);
    return j;
}

}  // namespace rfmap
