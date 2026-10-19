# rfmod: the module platform (ABI, SDK, signed packages, registry, isolated worker, CLI).

include(FetchContent)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(LibArchive REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)

set(JSON_VALIDATOR_INSTALL OFF CACHE BOOL "" FORCE)
set(JSON_VALIDATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(JSON_VALIDATOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(json_schema_validator
  GIT_REPOSITORY https://github.com/pboettch/json-schema-validator.git
  GIT_TAG 2.4.1
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(json_schema_validator)
# Third-party code: do not hold it to our warning flags.
if(TARGET nlohmann_json_schema_validator)
  target_compile_options(nlohmann_json_schema_validator PRIVATE -w)
endif()

# The owner's public key is part of the build. Generate it once with `rfmod keygen` and pass
# -DRFMAP_OWNER_PUBKEY="rfmod-ed25519-pub:...". Without it the development key is used, which
# is public in this repository and must never sign anything that reaches a real server.
set(RFMAP_OWNER_PUBKEY "" CACHE STRING "Owner Ed25519 public key (rfmod-ed25519-pub:...)")
set(RFMAP_DEV_OWNER_PUBKEY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/keys/dev-owner.pub")
if(RFMAP_OWNER_PUBKEY STREQUAL "")
  file(READ "${RFMAP_DEV_OWNER_PUBKEY_FILE}" _dev_key)
  string(STRIP "${_dev_key}" RFMAP_EFFECTIVE_OWNER_PUBKEY)
  set(RFMAP_OWNER_KEY_IS_DEV 1)
  message(WARNING "RFMAP_OWNER_PUBKEY is not set: building with the public development owner key.")
else()
  set(RFMAP_EFFECTIVE_OWNER_PUBKEY "${RFMAP_OWNER_PUBKEY}")
  set(RFMAP_OWNER_KEY_IS_DEV 0)
endif()

set(_schema "${PROJECT_SOURCE_DIR}/schemas/rfmod-manifest.schema.json")
set(_schema_header "${CMAKE_CURRENT_BINARY_DIR}/generated/manifest_schema.hpp")
add_custom_command(OUTPUT "${_schema_header}"
  COMMAND ${CMAKE_COMMAND} -DINPUT=${_schema} -DOUTPUT=${_schema_header} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_schema.cmake
  DEPENDS "${_schema}" ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_schema.cmake
  COMMENT "Embedding rfmod manifest schema")

# Header-only SDK for module authors.
add_library(rfmod_sdk INTERFACE)
target_include_directories(rfmod_sdk INTERFACE include)
target_link_libraries(rfmod_sdk INTERFACE nlohmann_json::nlohmann_json)
# GCC marks libraries with "unique" symbols (inline statics, templates) as never unloadable, so
# dlclose would keep an old version mapped after an upgrade. Modules built with the SDK opt out.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(rfmod_sdk INTERFACE -fno-gnu-unique)
endif()

add_library(rfmod_platform
  src/semver.cpp
  src/crypto.cpp
  src/manifest.cpp
  src/trust.cpp
  src/package.cpp
  src/in_process_module.cpp
  src/isolated_module.cpp
  src/worker_exec.cpp
  src/confined_root.cpp
  src/registry.cpp
  "${_schema_header}")
target_include_directories(rfmod_platform PUBLIC include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(rfmod_platform
  PUBLIC nlohmann_json::nlohmann_json
  PRIVATE nlohmann_json_schema_validator LibArchive::LibArchive PkgConfig::SODIUM ${CMAKE_DL_LIBS})
target_compile_definitions(rfmod_platform PUBLIC
  RFMAP_OWNER_PUBKEY_TEXT="${RFMAP_EFFECTIVE_OWNER_PUBKEY}"
  RFMAP_OWNER_KEY_IS_DEV=${RFMAP_OWNER_KEY_IS_DEV}
  RFMAP_HOST_VERSION="${PROJECT_VERSION}")

add_executable(rfmod-worker tools/rfmod-worker.cpp)
target_link_libraries(rfmod-worker PRIVATE rfmod_sdk ${CMAKE_DL_LIBS})

add_executable(rfmod tools/rfmod-cli.cpp)
target_link_libraries(rfmod PRIVATE rfmod_platform)
add_dependencies(rfmod rfmod-worker)

if(RFMAP_BUILD_TESTS)
  add_subdirectory(tests)
endif()
