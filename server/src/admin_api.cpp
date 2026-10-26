#include "admin_api.hpp"

namespace rfmap {

void AdminApi::updateInstalled(const std::string& id, const json& b) const {
    const ModuleGate::Change gate(s_.moduleGate());  // the settings are a file under the module root
    try {
        if (b.contains("options")) s_.registry().setOptions(id, b.at("options"));
        if (b.contains("enabled")) s_.registry().setEnabled(id, b.at("enabled").get<bool>());
    } catch (const std::invalid_argument& e) {
        throw HttpError(422, e.what());
    }
}

void AdminApi::announceModules() const {
    if (s_.settings().publish(SettingsTopic::kModules)) return;
    throw HttpError(503, "applied on this server process, but the others could not be told; "
                         "retry, or POST /admin/settings/reload once the database answers");
}

void AdminApi::putModule(const HttpRequest& req, HttpReply& res) const {
    admin_.require(req);
    const std::string id = req.match(1);
    const json b = req.body();
    if (!b.is_object()) throw HttpError(422, "send {enabled?, options?}");
    if (b.contains("enabled") && !b.at("enabled").is_boolean()) throw HttpError(422, "enabled must be a boolean");
    if (s_.builtins().isBuiltin(id)) {
        std::optional<bool> enabled;
        if (b.contains("enabled")) enabled = b.at("enabled").get<bool>();
        std::optional<json> options;
        if (b.contains("options")) options = b.at("options");
        s_.builtins().set(id, enabled, options);
        // Unannounced, the change still reaches the others by the next fingerprint poll.
        (void)s_.settings().publish(SettingsTopic::kBuiltins);
    } else {
        updateInstalled(id, b);
        announceModules();
    }
    res.send(200, s_.catalog().list(true));
}

void AdminApi::postModulesReload(const HttpRequest& req, HttpReply& res) const {
    admin_.require(req);
    s_.reloadModules();
    announceModules();
    s_.log().info("modules", "reloaded the module root and trust store on request");
    res.send(200, {{"modules", s_.catalog().list(true)}, {"notices", s_.registry().notices()}});
}

void AdminApi::getNotices(const HttpRequest& req, HttpReply& res) const {
    admin_.require(req);
    res.send(200, s_.registry().notices());
}

json AdminApi::trustedKeys() const {
    json out = json::array();
    for (const auto& k : s_.trust().keys()) {
        out.push_back({{"id", k.key.id()}, {"label", k.label}, {"key", k.key.toText()}, {"builtIn", k.builtIn},
                       {"firstParty", k.firstParty()}});
    }
    return out;
}

void AdminApi::getKeys(const HttpRequest& req, HttpReply& res) const {
    admin_.require(req);
    res.send(200, trustedKeys());
}

void AdminApi::getCache(const HttpRequest& req, HttpReply& res) const {
    admin_.require(req);
    res.send(200, s_.caches().tiers());
}

void AdminApi::putCache(const HttpRequest& req, HttpReply& res) const {
    admin_.require(req);
    s_.caches().setLimits(req.body());
    (void)s_.settings().publish(SettingsTopic::kCacheLimits);  // the poll covers it, as above
    res.send(200, s_.caches().tiers());
}

void AdminApi::deleteCache(const HttpRequest& req, HttpReply& res) const {
    admin_.require(req);
    s_.caches().clear(req.hasParam("tier") ? req.param("tier") : "all");
    res.noContent();
}

void AdminApi::getMetrics(const HttpRequest& req, HttpReply& res) const {
    admin_.require(req);
    if (!s_.config().metrics.enabled) throw HttpError(404, "metrics are off; set metrics.enabled in the server config");
    res.text(200, metrics_(), "text/plain; version=0.0.4; charset=utf-8");
}

void AdminApi::getSettingsSync(const HttpRequest& req, HttpReply& res) const {
    admin_.require(req);
    res.send(200, s_.settings().status());
}

void AdminApi::postSettingsReload(const HttpRequest& req, HttpReply& res) const {
    admin_.require(req);
    const std::string scope = req.hasParam("scope") ? req.param("scope") : "all";
    if (scope != "all" && scope != "local") throw HttpError(422, "scope is all or local");
    const auto outcome = s_.settings().reloadAll(scope == "all");
    if (!outcome.reloaded) {
        throw HttpError(503, "some settings could not be reloaded on this server process (see its log); retry",
                        {{"announced", outcome.announced}});
    }
    if (!outcome.announced) {
        throw HttpError(503, "reloaded this server process, but the others could not be told; retry");
    }
    s_.log().info("settings", "reloaded the settings on request, scope " + scope);
    res.send(200, s_.settings().status());
}

}  // namespace rfmap
