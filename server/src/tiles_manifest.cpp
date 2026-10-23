#include "tiles_manifest.hpp"

#include <algorithm>
#include <fstream>

namespace rfmap {

namespace {

/// The "source" of `name` in a manifest's "files": tiles-builder writes
/// {"files": {"<name>": {"source": ..., ...}}} or a list with "name" or "file".
std::string sourceInFiles(const nlohmann::json& files, const std::string& name) {
    if (files.is_object() && files.contains(name)) return files.at(name).value("source", "");
    if (!files.is_array()) return {};
    for (const auto& f : files) {
        if (f.value("name", "") == name || f.value("file", "") == name) return f.value("source", "");
    }
    return {};
}

}  // namespace

TilesManifest::TilesManifest(const std::filesystem::path& dataFile) : fileName_(dataFile.filename().string()) {
    std::ifstream in(dataFile.parent_path() / "manifest.json");
    if (!in) return;
    try {
        document_ = nlohmann::json::parse(in);
    } catch (const nlohmann::json::exception&) {
        document_.reset();  // not JSON: the same as no manifest
    }
}

std::string TilesManifest::sourceName() const {
    if (!document_ || !document_->contains("files")) return {};
    try {
        return sourceInFiles(document_->at("files"), fileName_);
    } catch (const nlohmann::json::exception&) {
        return {};  // an entry of the wrong type: the source is unknown
    }
}

std::vector<std::string> TilesManifest::attributions() const {
    std::vector<std::string> out;
    if (!document_) return out;
    try {
        if (document_->contains("files") && document_->at("files").is_array()) {
            for (const auto& f : document_->at("files")) {
                const std::string a = f.value("attribution", "");
                if (!a.empty() && std::ranges::find(out, a) == out.end()) out.push_back(a);
            }
        }
        if (out.empty()) {
            if (const std::string joined = document_->value("attribution_display_text", ""); !joined.empty()) {
                out.push_back(joined);
            }
        }
    } catch (const nlohmann::json::exception&) {
        return out;  // an entry of the wrong type: keep what was read before it
    }
    return out;
}

std::optional<std::set<std::string, std::less<>>> TilesManifest::files() const {
    if (!document_ || !document_->contains("files") || !document_->at("files").is_array()) return std::nullopt;
    try {
        std::set<std::string, std::less<>> out;
        for (const auto& f : document_->at("files")) {
            const std::string name = f.value("file", "");
            if (!name.empty()) out.insert(name);
        }
        return out;
    } catch (const nlohmann::json::exception&) {  // an entry that is not an object
        return std::nullopt;
    }
}

}  // namespace rfmap
