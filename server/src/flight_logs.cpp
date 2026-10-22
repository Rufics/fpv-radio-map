#include "flight_logs.hpp"

#include "security.hpp"

#include <rfmod/crypto.hpp>

#include <algorithm>
#include <format>
#include <mutex>
#include <set>
#include <stdexcept>

namespace rfmap {
namespace {

constexpr std::size_t kMaxFileName = 120;
/// What a decoder sees to tell whether a file is its format: only the full decode gets every byte.
constexpr std::size_t kProbeBytes = 65'536;

/// Bytes of the valid UTF-8 character at `i`, or 0 when the bytes there are not one.
std::size_t utf8Length(std::string_view s, std::size_t i) {
    const unsigned lead = static_cast<unsigned char>(s[i]);
    std::size_t n = 0;
    if (lead < 0x80) n = 1;
    else if (lead >= 0xC2 && lead <= 0xDF) n = 2;
    else if (lead >= 0xE0 && lead <= 0xEF) n = 3;
    else if (lead >= 0xF0 && lead <= 0xF4) n = 4;
    if (n == 0 || i + n > s.size()) return 0;
    if (n == 1) return 1;
    // The second byte's range also rules out overlong forms, surrogates and code points past U+10FFFF.
    const unsigned second = static_cast<unsigned char>(s[i + 1]);
    unsigned low = 0x80;
    unsigned high = 0xBF;
    if (lead == 0xE0) low = 0xA0;
    else if (lead == 0xED) high = 0x9F;
    else if (lead == 0xF0) low = 0x90;
    else if (lead == 0xF4) high = 0x8F;
    if (second < low || second > high) return 0;
    for (std::size_t k = 2; k < n; ++k) {
        const unsigned next = static_cast<unsigned char>(s[i + k]);
        if ((next & 0xC0u) != 0x80u) return 0;
    }
    return n;
}

std::optional<std::string> startedAtOf(const json& entry) {
    if (entry.contains("startedAt") && entry.at("startedAt").is_string()) return entry.at("startedAt").get<std::string>();
    return std::nullopt;
}

}  // namespace

// ---- store ----

std::ptrdiff_t FlightLogStore::countNew(pqxx::work& w, const std::string& missionId, const std::string& sha256,
                                        const std::vector<NewFlight>& flights) {
    std::set<int> known;
    for (const auto& r : w.exec("select log_index from flight_logs where mission_id = $1 and file_sha256 = $2",
                                pqxx::params{missionId, sha256})) {
        known.insert(r[0].as<int>());
    }
    return std::ranges::count_if(flights, [&known](const NewFlight& f) { return !known.contains(f.index); });
}

std::string FlightLogStore::insert(pqxx::work& w, const FileRef& file, const NewFlight& f) {
    const auto r = w.exec(
        "insert into flight_logs(id, mission_id, file_name, file_sha256, log_index, decoder, started_at, "
        "duration_s, normalized, summary) values ($1, $2, $3, $4, $5, $6, $7, $8, $9::jsonb, $10::jsonb) "
        "on conflict (mission_id, file_sha256, log_index) do update set file_name = excluded.file_name "
        "returning id",
        pqxx::params{Secrets::randomToken(12), file.missionId, file.fileName, file.sha256, f.index, file.decoder,
                     f.startedAt, f.durationS, f.normalized.dump(), f.summary.dump()});
    return r[0][0].as<std::string>();
}

std::optional<std::vector<std::string>> FlightLogStore::add(const std::string& missionId, const std::string& fileName,
                                                            const std::string& sha256, const std::string& decoder,
                                                            const std::vector<NewFlight>& flights, int maxPerMission) {
    return db_.tx([&missionId, &fileName, &sha256, &decoder, &flights, maxPerMission](pqxx::work& w)
                      -> std::optional<std::vector<std::string>> {
        // Imports into one mission take turns, so two of them cannot both pass the limit.
        w.exec("select 1 from missions where id = $1 for update", pqxx::params{missionId});
        if (const int stored =
                w.exec("select count(*) from flight_logs where mission_id = $1", pqxx::params{missionId})[0][0].as<int>();
            stored + countNew(w, missionId, sha256, flights) > maxPerMission) {
            return std::nullopt;
        }
        std::vector<std::string> ids;
        ids.reserve(flights.size());
        for (const NewFlight& f : flights) ids.push_back(insert(w, {missionId, fileName, sha256, decoder}, f));
        return ids;
    });
}

json FlightLogStore::list(const std::string& missionId) const {
    return db_.tx([&missionId](pqxx::work& w) {
        json out = json::array();
        const auto rows = w.exec(
            "select id, file_name, decoder, summary::text, created_at from flight_logs where mission_id = $1 "
            "order by created_at desc, log_index",
            pqxx::params{missionId});
        for (const auto& r : rows) {
            json row = json::parse(r[3].as<std::string>());
            row["id"] = r[0].as<std::string>();
            row["fileName"] = r[1].as<std::string>();
            row["decoder"] = r[2].as<std::string>();
            row["importedAt"] = r[4].as<std::string>();
            out.push_back(std::move(row));
        }
        return out;
    });
}

std::optional<FlightLogStore::Stored> FlightLogStore::get(const std::string& missionId, const std::string& id) const {
    return db_.tx([&missionId, &id](pqxx::work& w) -> std::optional<Stored> {
        const auto r = w.exec("select file_name, decoder, normalized::text from flight_logs where mission_id = $1 and id = $2",
                              pqxx::params{missionId, id});
        if (r.empty()) return std::nullopt;
        return Stored{id, r[0][0].as<std::string>(), r[0][1].as<std::string>(), json::parse(r[0][2].as<std::string>())};
    });
}

bool FlightLogStore::remove(const std::string& missionId, const std::string& id) {
    return db_.tx([&missionId, &id](pqxx::work& w) {
        return w.exec("delete from flight_logs where mission_id = $1 and id = $2", pqxx::params{missionId, id}).affected_rows() > 0;
    });
}

// ---- service ----

FlightLogService::FlightLogService(ModuleRunner& modules, const SchemaRegistry& schemas, FlightLogStore* store,
                                   const BuiltinModules& builtins, Limits limits)
    : modules_(modules), schemas_(schemas), store_(store), builtins_(builtins), limits_(limits) {}

std::string FlightLogService::cleanFileName(const std::string& name) {
    const std::size_t slash = name.find_last_of("/\\");
    std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
    // Keeps whole UTF-8 characters only, so the name stays valid JSON however it was sent or cut.
    std::string out;
    for (std::size_t i = 0; i < base.size();) {
        const std::size_t n = utf8Length(base, i);
        if (n == 0) {
            ++i;
            continue;
        }
        const auto c = static_cast<unsigned char>(base[i]);
        if (out.size() + n > kMaxFileName) break;
        if (n > 1 || (c >= 0x20 && c != 0x7f)) out.append(base, i, n);
        i += n;
    }
    return out.empty() ? "flight.log" : out;
}

void FlightLogService::requireEnabled() const {
    if (!builtins_.enabled(BuiltinModules::kFlightLog)) {
        throw HttpError(403, "flight log replay is switched off on this server (rf.flightlog)");
    }
}

void FlightLogService::requireStore() const {
    if (store_ == nullptr) throw HttpError(503, "flight logs need the database, which this server runs without");
}

rfflightlog::ProcessOptions FlightLogService::options() const {
    const json o = builtins_.options(BuiltinModules::kFlightLog);
    rfflightlog::ProcessOptions out;
    out.timelineHz = o.value("timelineHz", out.timelineHz);
    out.story.lowLinkPct = o.value("lowLinkPct", out.story.lowLinkPct);
    if (o.contains("lowCellV")) out.story.lowCellV = o.at("lowCellV").get<double>();
    return out;
}

json FlightLogService::decodeWith(rfmod::IModuleInstance& decoder, const json& request) {
    const std::string& id = decoder.manifest().id;
    try {
        return decoder.invoke("flightlog.decode", request);
    } catch (const rfmod::ModuleError& e) {
        if (e.code() == "bad_request") throw HttpError(422, id + " could not read the file: " + e.what(), {{"decoder", id}});
        throw HttpError(502, id + " failed on this file: " + e.what(), {{"decoder", id}, {"code", e.code()}});
    }
}

bool FlightLogService::probe(rfmod::IModuleInstance& decoder, const json& request) {
    // A decoder that fails on a file it was only asked about does not stop the others from trying it.
    try {
        const json answer = decoder.invoke("flightlog.probe", request);
        return answer.is_object() && answer.value("match", false);
    } catch (const rfmod::ModuleError&) {
        return false;
    }
}

FlightLogService::Decoded FlightLogService::decode(std::string_view bytes, const std::string& fileName) const {
    if (bytes.empty()) throw HttpError(400, "the request body is empty: send the log file's bytes");
    if (bytes.size() > limits_.maxBytes) {
        throw HttpError(413, std::format("the file is {} bytes; this server takes up to {}", bytes.size(), limits_.maxBytes));
    }
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const json question = {{"file", rfmod::toBase64(data, std::min(bytes.size(), kProbeBytes))}, {"fileName", fileName}};
    const ModuleGate::Read gate(modules_.gate());
    const rfmod::ModuleRegistry* registry = modules_.registry();
    const std::vector<rfmod::IModuleInstance*> decoders =
        registry ? registry->withCapability("flightlog.decoder") : std::vector<rfmod::IModuleInstance*>{};
    if (decoders.empty()) throw HttpError(422, "no flight log decoder module is installed on this server");
    json tried = json::array();
    for (rfmod::IModuleInstance* d : decoders) {
        tried.push_back(d->manifest().id);
        if (!probe(*d, question)) continue;
        json document = decodeWith(*d, {{"file", rfmod::toBase64(data, bytes.size())}, {"fileName", fileName}});
        if (const auto problems = schemas_.check("flightlog", document); !problems.empty()) {
            throw HttpError(502, d->manifest().id + " answered a document that does not match the flightlog schema: " +
                                     problems.front().pointer + " " + problems.front().message,
                            {{"decoder", d->manifest().id}});
        }
        return {d->manifest().id, std::move(document)};
    }
    throw HttpError(422, "none of the installed decoders recognises this file", {{"decoders", tried}});
}

json FlightLogService::single(const json& document, const json& entry) {
    return {{"schema", document.at("schema")}, {"source", document.at("source")}, {"logs", json::array({entry})}};
}

json FlightLogService::preview(const std::string& bytes, const std::string& fileName) const {
    requireEnabled();
    const Decoded d = decode(bytes, cleanFileName(fileName));
    json flights = json::array();
    for (const rfflightlog::ProcessedFlight& f : processor_.process(d.document, options())) flights.push_back(f.toJson());
    return {{"decoder", d.decoder}, {"source", d.document.at("source")}, {"flights", flights}};
}

json FlightLogService::importFile(const std::string& missionId, const std::string& bytes, const std::string& fileName) {
    requireEnabled();
    requireStore();
    const std::string name = cleanFileName(fileName);
    const Decoded d = decode(bytes, name);
    const auto processed = processor_.process(d.document, options());
    std::vector<FlightLogStore::NewFlight> rows;
    for (std::size_t i = 0; i < processed.size(); ++i) {
        const json& entry = d.document.at("logs").at(i);
        rows.emplace_back(entry.at("index").get<int>(), startedAtOf(entry), processed[i].log.durationS,
                          single(d.document, entry), processed[i].summaryJson());
    }
    const std::string sha = rfmod::Sha256::hex({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    const auto added = store_->add(missionId, name, sha, d.decoder, rows, limits_.maxPerMission);
    if (!added.has_value()) {
        throw HttpError(409, std::format("a mission keeps up to {} flights; delete some first", limits_.maxPerMission));
    }
    const std::vector<std::string>& ids = *added;
    json flights = json::array();
    for (std::size_t i = 0; i < ids.size(); ++i) {
        json row = rows[i].summary;
        row["id"] = ids[i];
        flights.push_back(std::move(row));
    }
    return {{"decoder", d.decoder}, {"fileName", name}, {"flights", flights}};
}

json FlightLogService::list(const std::string& missionId) const {
    requireStore();
    return store_->list(missionId);
}

json FlightLogService::view(const MissionRecord& mission, const std::string& id) const {
    requireEnabled();
    requireStore();
    const auto stored = store_->get(mission.id, id);
    if (!stored) throw HttpError(404, "no such flight log in this mission");
    const auto flights = processor_.process(stored->normalized, options());
    if (flights.empty()) throw StoredDataError("flight log " + id + " holds no flight");
    json out = flights.front().toJson();
    out["id"] = stored->id;
    out["fileName"] = stored->fileName;
    out["decoder"] = stored->decoder;
    const rfflightlog::RoutePlan plan = rfflightlog::RoutePlan::fromMission(mission.doc, mission.lastResult);
    out["plan"] = processor_.compare(flights.front(), plan);
    out["plan"]["evaluated"] = mission.lastResult.has_value();
    return out;
}

void FlightLogService::remove(const std::string& missionId, const std::string& id) {
    requireStore();
    if (!store_->remove(missionId, id)) throw HttpError(404, "no such flight log in this mission");
}

}  // namespace rfmap
