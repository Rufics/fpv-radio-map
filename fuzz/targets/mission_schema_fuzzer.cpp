// Request validation (server schemas.hpp): every mission and library item a client sends is
// checked against the draft-07 schemas under schemas/ before any code reads it.
//
// Input: a JSON document. Property: check() never throws, whatever the document.

#include "../support.hpp"

#include "schemas.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const rfmap::SchemaRegistry schemas(RFMAP_SCHEMAS_DIR);
    const rfmap::json doc = rfmap::json::parse(data, data + size, nullptr, false);
    if (doc.is_discarded()) return 0;

    (void)schemas.check("mission", doc);
    for (const std::string& kind : rfmap::SchemaRegistry::libraryKinds()) (void)schemas.check(kind, doc);
    return 0;
}
