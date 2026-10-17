#include "missions.hpp"

namespace rfmap {

std::string MissionStore::create(const json& doc, const Digest& ownerKeyHash) {
    const std::string id = Secrets::randomToken(12);
    db_.tx([&](pqxx::work& w) {
        w.exec_params(
            "insert into missions(id, owner_key_hash, name, doc, pilot) "
            "values ($1, decode($2,'hex'), $3, $4::jsonb, ST_SetSRID(ST_MakePoint($5, $6), 4326)::geography)",
            id, Secrets::hex(ownerKeyHash), doc.at("name").get<std::string>(), doc.dump(),
            doc.at("pilot").at("lon").get<double>(), doc.at("pilot").at("lat").get<double>());
    });
    return id;
}

std::optional<MissionRecord> MissionStore::get(const std::string& id) const {
    return db_.tx([&](pqxx::work& w) -> std::optional<MissionRecord> {
        const auto r = w.exec_params("select doc::text, revision, last_result::text from missions where id = $1", id);
        if (r.empty()) return std::nullopt;
        MissionRecord m{id, json::parse(r[0][0].as<std::string>()), r[0][1].as<int>(), std::nullopt};
        if (!r[0][2].is_null()) m.lastResult = json::parse(r[0][2].as<std::string>());
        return m;
    });
}

std::optional<int> MissionStore::save(const std::string& id, const json& doc, int expected) {
    return db_.tx([&](pqxx::work& w) -> std::optional<int> {
        const auto r = w.exec_params(
            "update missions set doc = $2::jsonb, name = $3, revision = revision + 1, last_result = null, updated_at = now(), "
            "pilot = ST_SetSRID(ST_MakePoint($5, $6), 4326)::geography "
            "where id = $1 and revision = $4 returning revision",
            id, doc.dump(), doc.at("name").get<std::string>(), expected, doc.at("pilot").at("lon").get<double>(),
            doc.at("pilot").at("lat").get<double>());
        if (r.empty()) return std::nullopt;
        return r[0][0].as<int>();
    });
}

void MissionStore::storeResultIfCurrent(const std::string& id, const json& doc, const json& result) {
    // jsonb equality ignores key order and whitespace, so this matches the saved mission exactly.
    db_.tx([&](pqxx::work& w) {
        w.exec_params("update missions set last_result = $3::jsonb where id = $1 and doc = $2::jsonb", id, doc.dump(),
                      result.dump());
    });
}

}  // namespace rfmap
