// fpv.blackbox module: host glue and self-test. Decodes Betaflight Blackbox files into the
// normalized flight log; the server runs it in an isolated worker because it reads files that
// users upload.

#include "decoder.hpp"
#include "sample_log.hpp"

#include <rfmod/module.hpp>

#include <cmath>
#include <format>

namespace fpv::blackbox {
namespace {

constexpr const char* kManifest = R"json({"id":"fpv.blackbox","version":"1.0.0"})json";
constexpr const char* kId = "fpv.blackbox";
constexpr const char* kVersion = "1.0.0";

bool near(const json& v, double expected) { return v.is_number() && std::abs(v.get<double>() - expected) < 1e-9; }

}  // namespace

class BlackboxModule final : public rfmod::Module {
public:
    const char* manifest() const override { return kManifest; }

    json invoke(const std::string& method, const json& request) override {
        if (method == "flightlog.probe") return decoder_.probe(file(request));
        if (method == "flightlog.decode") {
            const std::vector<std::uint8_t> bytes = file(request);
            return decoder_.decode(bytes, request.value("fileName", std::string()), options(request));
        }
        throw rfmod::UnknownMethod(method);
    }

    json selftest() override {
        BuildOptions opts;
        opts.sampleHz = 1000.0;
        const json log = decoder_.decode(SampleLog::build(), "selftest.bbl", opts).at("logs").at(0);
        const json& series = log.at("series");
        const json& gps = log.at("gps");
        const bool frames = log.at("status") == "ok" && series.at("t").size() == 2 && near(series.at("t").at(1), 0.001);
        const bool battery = near(series.at("voltageV").at(0), 16.0) && near(series.at("voltageV").at(1), 15.98);
        const bool position = near(gps.at("lat").at(0), 50.00001) && near(gps.at("lon").at(0), 30.00002) &&
                              near(gps.at("altM").at(0), 123.4) && near(gps.at("speedMps").at(0), 10.0);
        return {{"ok", frames && battery && position},
                {"checks",
                 {{{"name", "main frames and predictors"}, {"ok", frames}, {"detail", series.at("t").dump()}},
                  {{"name", "battery voltage (4.5.1 units)"}, {"ok", battery}, {"detail", series.at("voltageV").dump()}},
                  {{"name", "GPS relative to home"}, {"ok", position},
                   {"detail", std::format("{} {}", gps.at("lat").dump(), gps.at("lon").dump())}}}}};
    }

private:
    static std::vector<std::uint8_t> file(const json& request) {
        if (!request.contains("file") || !request.at("file").is_string()) throw rfmod::BadRequest("file (base64) is required");
        auto bytes = Base64::decode(request.at("file").get_ref<const std::string&>());
        if (!bytes) throw rfmod::BadRequest("file is not valid base64");
        return std::move(*bytes);
    }

    static BuildOptions options(const json& request) {
        BuildOptions opts;
        const json o = request.value("options", json::object());
        if (!o.is_object()) throw rfmod::BadRequest("options must be an object");
        if (o.contains("sampleHz")) {
            if (!o.at("sampleHz").is_number()) throw rfmod::BadRequest("options.sampleHz must be a number");
            const double hz = o.at("sampleHz").get<double>();
            if (!(hz >= 0.1 && hz <= 1000.0)) throw rfmod::BadRequest("options.sampleHz must be within 0.1 to 1000");
            opts.sampleHz = hz;
        }
        return opts;
    }

    BlackboxDecoder decoder_{kId, kVersion};
};

}  // namespace fpv::blackbox

RFMOD_DEFINE_MODULE(fpv::blackbox::BlackboxModule)
