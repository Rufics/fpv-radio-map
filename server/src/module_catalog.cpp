#include "module_catalog.hpp"

namespace rfmap {

ModuleCatalog::ModuleCatalog(const BuiltinModules& builtins, const rfmod::ModuleRegistry& registry,
                             const BundleGrants& grants)
    : builtins_(builtins), registry_(registry), grants_(grants) {}

std::string ModuleCatalog::fileUrl(const std::string& id, const std::string& path) {
    return "/api/v1/modules/" + id + "/" + path;
}

std::string ModuleCatalog::contentType(std::string_view ext) {
    if (ext == ".js" || ext == ".mjs") return "text/javascript";
    if (ext == ".json") return "application/json";
    if (ext == ".css") return "text/css";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    return "application/octet-stream";
}

void ModuleCatalog::describeInstalled(json& m, bool admin) const {
    m["builtIn"] = false;
    if (!m.contains("ui")) return;
    const std::string id = m.at("id").get<std::string>();
    if (m.value("lifecycle", "") != "alpha") {
        m["ui"]["bundle"] = fileUrl(id, m["ui"]["bundle"].get<std::string>());
        return;
    }
    if (!admin) {
        m.erase("ui");
        return;
    }
    const std::string grant = grants_.issue(id, Clock::now() + kGrantLifetime);
    m["ui"]["bundle"] = fileUrl(id, m["ui"]["bundle"].get<std::string>()) + "?grant=" + grant;
}

json ModuleCatalog::list(bool admin) const {
    json out = builtins_.list();
    for (json& m : registry_.list()) {
        describeInstalled(m, admin);
        out.push_back(std::move(m));
    }
    return out;
}

std::optional<ModuleCatalog::PublicFile> ModuleCatalog::file(const std::string& id, const std::string& path) const {
    // One registry call gives the bytes and the lifecycle of one module version, read through
    // the directory the version was loaded from, so neither a reload nor an install that swaps
    // the directory on disk can pair a new alpha bundle with an old public lifecycle.
    auto found = registry_.publicFile(id, path);
    if (!found) return std::nullopt;
    return PublicFile{std::move(found->content), contentType(found->path.extension().string()), found->lifecycle == rfmod::Lifecycle::Alpha};
}

bool ModuleCatalog::grantValid(const std::string& id, std::string_view grant, TimePoint now) const {
    return grants_.valid(id, grant, now);
}

}  // namespace rfmap
