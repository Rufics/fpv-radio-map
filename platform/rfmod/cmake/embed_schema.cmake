# Turns schemas/rfmod-manifest.schema.json into a header so the host validates manifests with
# exactly the schema in the repository.
file(READ "${INPUT}" CONTENT)
file(WRITE "${OUTPUT}" "#pragma once\n// Generated from ${INPUT}. Do not edit.\ninline constexpr const char* kManifestSchema = R\"rfmodschema(${CONTENT})rfmodschema\";\n")
