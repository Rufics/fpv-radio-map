# Fuzz targets, built with -DRFMAP_BUILD_FUZZERS=ON (see docs/dev/fuzzing.md). Each target gets a
# ctest test, label "fuzz", that runs it once over its corpus: the seeds and every input that once
# found a bug, so a fixed bug stays fixed.

find_package(LibArchive REQUIRED)

add_custom_target(rfmap_fuzzers)

# rfmap_fuzzer(<name> LIBS ... [DEFS ...]): fuzz/targets/<name>_fuzzer.cpp, corpus fuzz/corpus/<name>/.
function(rfmap_fuzzer name)
  cmake_parse_arguments(F "" "" "LIBS;DEFS" ${ARGN})
  set(target ${name}_fuzzer)
  add_executable(${target} targets/${target}.cpp)
  target_link_libraries(${target} PRIVATE ${F_LIBS})
  target_compile_definitions(${target} PRIVATE ${F_DEFS} RFMAP_FUZZ_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
  set(corpus ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name})
  if(RFMAP_FUZZ_LIBFUZZER)
    target_link_options(${target} PRIVATE ${RFMAP_FUZZ_ENGINE_FLAGS})
    add_test(NAME fuzz.${name}.corpus COMMAND ${target} -runs=0 ${corpus})
  else()
    target_sources(${target} PRIVATE standalone_main.cpp)
    target_compile_definitions(${target} PRIVATE RFMAP_FUZZ_CORPUS="${corpus}")
    add_test(NAME fuzz.${name}.corpus COMMAND ${target})
  endif()
  set_tests_properties(fuzz.${name}.corpus PROPERTIES LABELS fuzz)
  add_dependencies(rfmap_fuzzers ${target})
endfunction()

# Module platform: everything an uploaded package or a manifest carries.
rfmap_fuzzer(semver LIBS rfmod_platform)
rfmap_fuzzer(crypto_text LIBS rfmod_platform)
rfmap_fuzzer(manifest LIBS rfmod_platform)
rfmap_fuzzer(package LIBS rfmod_platform)
rfmap_fuzzer(signed_package LIBS rfmod_platform LibArchive::LibArchive
  DEFS RFMAP_FUZZ_DEV_OWNER_SECRET="${PROJECT_SOURCE_DIR}/platform/rfmod/keys/dev-owner.secret")

# Radio maths on arbitrary terrain.
rfmap_fuzzer(link_budget LIBS rf_ext)

# Server: request bodies and the forecast service's answers.
if(TARGET rfmap_server_lib)
  rfmap_fuzzer(iso_time LIBS rfmap_server_lib)
  rfmap_fuzzer(weather LIBS rfmap_server_lib)
  rfmap_fuzzer(mission_schema LIBS rfmap_server_lib DEFS RFMAP_SCHEMAS_DIR="${PROJECT_SOURCE_DIR}/schemas")
endif()
