// Module manifest validation (rfmod/manifest.hpp): the JSON schema plus the rules it cannot
// express. The parser reads fields with nlohmann::json::at/get after the schema passed, so any
// manifest the schema lets through and the parser then throws on is a bug in one of the two.
//
// Input: manifest.json bytes. Property: parse() never throws, it reports.

#include "../support.hpp"

#include <rfmod/manifest.hpp>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const rfmod::Version host = rfmod::Version::parse(RFMAP_HOST_VERSION);
    const nlohmann::json doc = nlohmann::json::parse(data, data + size, nullptr, false);
    if (doc.is_discarded()) return 0;

    rfmod::ValidationReport report;
    const auto manifest = rfmod::ManifestParser::parse(doc, host, report);
    RFMAP_FUZZ_CHECK(manifest.has_value() == report.ok());
    if (manifest.has_value()) {
        RFMAP_FUZZ_CHECK(rfmod::Version::parse(manifest->version.str()) == manifest->version);
    }
    return 0;
}
