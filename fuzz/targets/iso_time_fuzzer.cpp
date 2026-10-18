// ISO 8601 times from requests (server common.hpp): mission times, calc requests, link expiry
// and host calls of modules all go through parseIsoUtc.
//
// Property: a parsed time prints (isoUtc) as a time that parses back to the same second.

#include "../support.hpp"

#include "common.hpp"

#include <chrono>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);
    const auto t = rfmap::parseIsoUtc(text);
    if (!t.has_value()) return 0;
    const std::string printed = rfmap::isoUtc(*t);
    const auto again = rfmap::parseIsoUtc(printed);
    RFMAP_FUZZ_CHECK(again.has_value());
    RFMAP_FUZZ_CHECK(*again == std::chrono::floor<std::chrono::seconds>(*t));
    return 0;
}
