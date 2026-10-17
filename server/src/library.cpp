#include "library.hpp"

#include "security.hpp"

#include <fstream>
#include <sstream>

namespace rfmap {

namespace {
void checkKind(const std::string& kind) { (void)SchemaRegistry::libraryFile(kind); }

/** Writes `doc` as the current item and as its history row `version`. */
void storeVersion(pqxx::work& w, const std::string& kind, const std::string& id, int version, const json& doc,
                  const std::string& by) {
    // Non-string parameters go in as the text pqxx itself would send: passing the int and bool
    // directly makes GCC 13 warn maybe-uninitialized inside pqxx::params' std::variant.
    const std::string verified = pqxx::to_string(doc.at("verified").get<bool>());
    const std::string next = pqxx::to_string(version);
    const std::string body = doc.dump();
    w.exec_params(
        "update library_items set version = $3, verified = $4, doc = $5::jsonb, updated_at = now(), updated_by = $6 "
        "where kind = $1 and id = $2",
        kind, id, next, verified, body, by);
    w.exec_params(
        "insert into library_history(kind, id, version, verified, doc, changed_by) values ($1, $2, $3, $4, $5::jsonb, $6)",
        kind, id, next, verified, body, by);
}
}  // namespace

std::vector<json> Library::list(const std::string& kind) const {
    checkKind(kind);
    return db_.tx([&](pqxx::work& w) {
        std::vector<json> out;
        for (const auto& r : w.exec_params("select doc::text from library_items where kind = $1 order by id", kind)) {
            out.push_back(json::parse(r[0].as<std::string>()));
        }
        return out;
    });
}

std::optional<json> Library::get(const std::string& kind, const std::string& id) const {
    checkKind(kind);
    return db_.tx([&](pqxx::work& w) -> std::optional<json> {
        const auto r = w.exec_params("select doc::text from library_items where kind = $1 and id = $2", kind, id);
        if (r.empty()) return std::nullopt;
        return json::parse(r[0][0].as<std::string>());
    });
}

std::vector<json> Library::history(const std::string& kind, const std::string& id) const {
    checkKind(kind);
    return db_.tx([&](pqxx::work& w) {
        std::vector<json> out;
        for (const auto& r : w.exec_params(
                 "select version, verified, doc::text, extract(epoch from changed_at)::float8, changed_by "
                 "from library_history where kind = $1 and id = $2 order by version",
                 kind, id)) {
            const auto t = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(r[3].as<double>()));
            out.push_back({{"version", r[0].as<int>()},
                           {"verified", r[1].as<bool>()},
                           {"item", json::parse(r[2].as<std::string>())},
                           {"changedAt", isoUtc(TimePoint(t))},
                           {"changedBy", r[4].as<std::string>()}});
        }
        return out;
    });
}

json Library::add(const std::string& kind, json doc, const std::string& by) {
    checkKind(kind);
    if (!doc.is_object()) throw HttpError(422, "library item must be a JSON object");
    doc["version"] = 1;
    schemas_.require(kind, doc);
    const std::string id = doc.at("id").get<std::string>();
    const std::string verified = pqxx::to_string(doc.at("verified").get<bool>());
    db_.tx([&](pqxx::work& w) {
        const auto r = w.exec_params(
            "insert into library_items(kind, id, version, verified, doc, updated_by) values ($1, $2, 1, $3, $4::jsonb, $5) "
            "on conflict do nothing returning id",
            kind, id, verified, doc.dump(), by);
        if (r.empty()) throw HttpError(409, kind + "/" + id + " exists; edit it with PUT instead");
        w.exec_params(
            "insert into library_history(kind, id, version, verified, doc, changed_by) values ($1, $2, 1, $3, $4::jsonb, $5)",
            kind, id, verified, doc.dump(), by);
    });
    return doc;
}

json Library::update(const std::string& kind, const std::string& id, json doc, const std::string& by) {
    checkKind(kind);
    if (!doc.is_object()) throw HttpError(422, "library item must be a JSON object");
    if (doc.value("id", "") != id) throw HttpError(422, "the item's id must match the URL");
    if (!doc.contains("version") || !doc.at("version").is_number_integer()) {
        throw HttpError(422, "send the version you edited in \"version\"");
    }
    const int edited = doc.at("version").get<int>();
    return db_.tx([&](pqxx::work& w) {
        const auto cur = w.exec_params("select version from library_items where kind = $1 and id = $2 for update", kind, id);
        if (cur.empty()) throw HttpError(404, kind + "/" + id + " does not exist; add it with POST");
        const int current = cur[0][0].as<int>();
        if (edited != current) {
            throw HttpError(409, "edited version " + std::to_string(edited) + " but the library holds " +
                                     std::to_string(current), {{"currentVersion", current}});
        }
        doc["version"] = current + 1;
        schemas_.require(kind, doc);
        storeVersion(w, kind, id, current + 1, doc, by);
        return doc;
    });
}

std::vector<std::string> Library::seed(const std::filesystem::path& dir) {
    std::vector<std::string> log;
    if (!std::filesystem::is_directory(dir)) return {"library seed directory " + dir.string() + " not found; nothing seeded"};
    for (const std::string& kind : SchemaRegistry::libraryKinds()) {
        const auto file = dir / kind / "seed.json";
        if (!std::filesystem::exists(file)) continue;
        std::ifstream in(file);
        std::ostringstream content;
        content << in.rdbuf();
        const std::string name = kind + "/seed.json";
        const std::string sha = Secrets::hex(Secrets::sha256(content.str()));
        const bool done = db_.tx([&](pqxx::work& w) {
            return !w.exec_params("select 1 from seeds_applied where name = $1", name).empty();
        });
        // Only the first start imports: later edits or deletions by the admin must stick.
        if (done) continue;
        std::size_t added = 0, skipped = 0;
        for (json item : json::parse(content.str())) {
            item["verified"] = false;
            item["version"] = 1;
            const auto problems = schemas_.check(kind, item);
            if (!problems.empty()) {
                log.push_back("seed " + name + ": " + item.value("id", "?") + " rejected: " + problems.front().pointer +
                              " " + problems.front().message);
                ++skipped;
                continue;
            }
            try {
                add(kind, item, "seed:" + name);
                ++added;
            } catch (const HttpError&) {
                ++skipped;  // already present
            }
        }
        db_.tx([&](pqxx::work& w) {
            w.exec_params("insert into seeds_applied(name, sha256) values ($1, $2) on conflict do nothing", name, sha);
        });
        log.push_back("seeded " + std::to_string(added) + " " + kind + " from " + file.string() + " (unverified)" +
                      (skipped ? ", " + std::to_string(skipped) + " skipped" : ""));
    }
    return log;
}

std::pair<json, LibraryRef> Library::require(const std::string& kind, const std::string& id) const {
    auto item = get(kind, id);
    if (!item) throw HttpError(422, "the library has no " + kind + " item '" + id + "'", {{"kind", kind}, {"id", id}});
    LibraryRef ref{kind, id, item->at("version").get<int>(), item->at("verified").get<bool>()};
    return {std::move(*item), ref};
}

}  // namespace rfmap
