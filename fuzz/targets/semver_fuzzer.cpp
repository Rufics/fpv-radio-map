// Version and range parsing (rfmod/semver.hpp). Manifests from any signer carry versions and
// ranges, and the registry compares them to pick upgrades and resolve dependencies.
//
// Input: "<version>\n<range>". Properties: a parsed version prints back to itself, and a range
// made of one comparator that includes its own version ("=", ">=", "<=", "^", "~") contains it.

#include "../support.hpp"

#include <rfmod/semver.hpp>

#include <stdexcept>

using rfmod::Version;
using rfmod::VersionRange;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text = rfmap::fuzz::asText(data, size);
    const std::size_t nl = text.find('\n');
    const std::string_view versionText = text.substr(0, nl);
    const std::string_view rangeText = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);

    const auto v = Version::tryParse(versionText);
    if (v.has_value()) {
        const Version again = Version::parse(v->str());
        RFMAP_FUZZ_CHECK(again == *v);
        RFMAP_FUZZ_CHECK(again.str() == v->str());
        RFMAP_FUZZ_CHECK((*v <=> Version(v->major(), v->minor(), v->patch())) != std::strong_ordering::greater);
    }

    try {
        const VersionRange r = VersionRange::parse(rangeText);
        if (v.has_value()) (void)r.contains(*v);
        (void)r.contains(Version(1, 0, 0));
    } catch (const std::invalid_argument&) {
    }

    // Single comparators that always include their own version.
    for (std::string_view op : {"=", ">=", "<=", "^", "~"}) {
        if (!v.has_value() || versionText.find('+') != std::string_view::npos) break;
        const std::string range = std::string(op) + v->str();
        try {
            RFMAP_FUZZ_CHECK(VersionRange::parse(range).contains(*v));
        } catch (const std::invalid_argument&) {
            // Refusing a range is fine; accepting one that excludes its own version is not.
        }
    }
    return 0;
}
