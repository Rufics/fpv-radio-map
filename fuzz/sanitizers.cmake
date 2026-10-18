# Compiler and linker flags of the fuzz build (RFMAP_BUILD_FUZZERS=ON). Included from the top-level
# CMakeLists.txt before any target, so every library the fuzz targets reach is instrumented too.
#
#   RFMAP_FUZZ_SANITIZERS  sanitizers for the whole tree; empty when CXXFLAGS already carry them,
#                          as on OSS-Fuzz and ClusterFuzzLite.
#   RFMAP_FUZZ_ENGINE      link flags of the fuzzing engine, $LIB_FUZZING_ENGINE there; with Clang
#                          the default is libFuzzer (-fsanitize=fuzzer).
#
# With a compiler other than Clang there is no libFuzzer: the targets are linked with
# standalone_main.cpp and only replay their corpus, still under the sanitizers.

set(RFMAP_FUZZ_SANITIZERS "address,undefined" CACHE STRING "Sanitizers for the fuzz build (empty: taken from CXXFLAGS)")
set(RFMAP_FUZZ_ENGINE "" CACHE STRING "Fuzzing engine link flags (default: -fsanitize=fuzzer with Clang)")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(RFMAP_FUZZ_LIBFUZZER ON)
  if(RFMAP_FUZZ_ENGINE STREQUAL "")
    set(RFMAP_FUZZ_ENGINE_FLAGS -fsanitize=fuzzer)
  else()
    separate_arguments(RFMAP_FUZZ_ENGINE_FLAGS UNIX_COMMAND "${RFMAP_FUZZ_ENGINE}")
  endif()
else()
  set(RFMAP_FUZZ_LIBFUZZER OFF)
  message(STATUS "Fuzz targets: ${CMAKE_CXX_COMPILER_ID} has no libFuzzer, building corpus replay programs")
endif()

if(NOT RFMAP_FUZZ_SANITIZERS STREQUAL "")
  # Any finding stops the run: a sanitizer report that lets the program continue is easy to miss.
  add_compile_options(-fsanitize=${RFMAP_FUZZ_SANITIZERS} -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${RFMAP_FUZZ_SANITIZERS})
  if(RFMAP_FUZZ_LIBFUZZER)
    # Coverage feedback in every library, not only in the fuzz target itself.
    add_compile_options(-fsanitize=fuzzer-no-link)
  endif()
endif()
