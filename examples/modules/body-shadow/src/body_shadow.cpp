// Example rfmod module: a user-supplied body shadow loss at the ground end.
//
// It shows the three things every loss contributor does: read options in init(), answer
// "loss.contribute" with named terms, and check itself in selftest(). It also shows how to be
// honest: the number is the user's, its source is shown next to it, and its spread is passed
// on as an uncertainty note instead of being folded into the value.

#include <rfmod/module.hpp>

#include <cmath>

namespace {

// Must match manifest.json; the host refuses the package otherwise.
constexpr const char* kManifest = R"json({"id":"example.body-shadow","version":"1.0.0"})json";

class BodyShadowModule final : public rfmod::Module {
public:
    const char* manifest() const override { return kManifest; }

    void init(const rfmod::HostClient& host, const rfmod::json& options) override {
        lossDb_ = options.value("lossDb", 0.0);
        source_ = options.value("source", std::string());
        uncertaintyDb_ = options.value("uncertaintyDb", 0.0);
        if (!std::isfinite(lossDb_) || lossDb_ < 0.0) throw rfmod::BadRequest("lossDb must be a finite number >= 0");
        host.log(RFMOD_LOG_INFO, "body shadow loss " + std::to_string(lossDb_) + " dB");
    }

    rfmod::json invoke(const std::string& method, const rfmod::json& request) override {
        if (method == "loss.contribute") return contribute(request);
        throw rfmod::UnknownMethod(method);
    }

    rfmod::json selftest() override {
        // The term must equal the option exactly and carry the module's id prefix.
        const double saved = lossDb_;
        lossDb_ = 3.5;
        const rfmod::json out = contribute(rfmod::json{{"frequencyHz", 5.8e9}});
        lossDb_ = saved;
        const rfmod::json& term = out.at("terms").at(0);
        const bool valueOk = term.at("valueDb").get<double>() == 3.5;
        const bool idOk = term.at("id").get<std::string>().starts_with("example.body-shadow/");
        return {{"ok", valueOk && idOk},
                {"checks", {{{"name", "value passes through unchanged"}, {"ok", valueOk}},
                            {{"name", "term id is namespaced"}, {"ok", idOk}}}}};
    }

private:
    rfmod::json contribute(const rfmod::json& request) const {
        if (!request.contains("frequencyHz")) throw rfmod::BadRequest("frequencyHz is required");
        rfmod::json notes = rfmod::json::array();
        if (uncertaintyDb_ > 0.0) {
            notes.push_back({{"what", "Body shadow"},
                             {"detail", "user value ±" + std::to_string(uncertaintyDb_) + " dB"}});
        }
        return {{"terms",
                 {{{"id", "example.body-shadow/ground"},
                   {"label", "Body shadow (ground end)"},
                   {"valueDb", lossDb_},
                   {"source", source_.empty() ? "user setting" : source_}}}},
                {"notes", notes}};
    }

    double lossDb_ = 0.0;
    std::string source_;
    double uncertaintyDb_ = 0.0;
};

}  // namespace

RFMOD_DEFINE_MODULE(BodyShadowModule)
