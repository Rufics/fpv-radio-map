#include "rfmod/trust.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace rfmod {

using json = nlohmann::json;

TrustStore::TrustStore(std::string_view ownerKeyText, std::filesystem::path storePath)
    : storePath_(std::move(storePath)) {
    keys_.emplace_back(PublicKey::fromText(ownerKeyText), "Owner (built in)", true);
    load();
}

std::optional<TrustedKey> TrustStore::find(std::string_view keyId) const {
    for (const TrustedKey& k : keys_) {
        if (k.key.id() == keyId) return k;
    }
    return std::nullopt;
}

const TrustedKey& TrustStore::add(const PublicKey& key, std::string label) {
    if (find(key.id())) throw std::invalid_argument("key " + key.id() + " is already trusted");
    if (label.empty()) throw std::invalid_argument("a key needs a label");
    keys_.emplace_back(key, std::move(label), false);
    save();
    return keys_.back();
}

void TrustStore::remove(const std::string& keyId) {
    const auto it = std::ranges::find_if(keys_, [&keyId](const TrustedKey& k) { return k.key.id() == keyId; });
    if (it == keys_.end()) throw std::invalid_argument("unknown key " + keyId);
    if (it->builtIn) throw std::invalid_argument("the built-in owner key cannot be removed");
    keys_.erase(it);
    save();
}

void TrustStore::load() {
    if (storePath_.empty() || !std::filesystem::exists(storePath_)) return;
    std::ifstream in(storePath_);
    const json doc = json::parse(in);
    for (const json& k : doc.at("keys")) {
        const PublicKey key = PublicKey::fromText(k.at("key").get<std::string>());
        if (!find(key.id())) keys_.emplace_back(key, k.at("label").get<std::string>(), false);
    }
}

void TrustStore::save() const {
    if (storePath_.empty()) return;
    json list = json::array();
    for (const TrustedKey& k : keys_) {
        if (!k.builtIn) list.push_back({{"id", k.key.id()}, {"key", k.key.toText()}, {"label", k.label}});
    }
    if (storePath_.has_parent_path()) std::filesystem::create_directories(storePath_.parent_path());
    const auto tmp = std::filesystem::path(storePath_.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << json{{"keys", list}}.dump(2) << '\n';
        if (!out) throw TrustStoreWriteError("cannot write " + tmp.string());
    }
    std::filesystem::rename(tmp, storePath_);
}

}  // namespace rfmod
