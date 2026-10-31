cmake_minimum_required(VERSION 3.25)
# The release workflow passes the version semantic-release computed from the commits.
set(RFMAP_VERSION "0.1.0" CACHE STRING "Version of this build (X.Y.Z)")
project(fpv_radio_map VERSION ${RFMAP_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

list(APPEND CMAKE_MODULE_PATH /usr/share/cmake/geographiclib)

option(RFMAP_BUILD_TESTS "Build unit tests" ON)
option(RFMAP_BUILD_SERVER "Build the HTTP server" ON)
option(RFMAP_COVERAGE "Instrument the build for line coverage (GCC or Clang, CI only)" OFF)
# Fuzz targets (fuzz/). With Clang they are libFuzzer binaries and the whole tree is built with
# AddressSanitizer and UndefinedBehaviorSanitizer; with other compilers they only replay the corpus.
option(RFMAP_BUILD_FUZZERS "Build fuzz targets" OFF)
# Google Benchmark binaries of the perf toolchain, one per folder in the catalog's
# extensions/benchmarks (tools/perf/README.md). Only perf.py turns this on.
option(RFMAP_BUILD_BENCHMARKS "Build the perf benchmarks" OFF)
set(RFMAP_PERF_CATALOG_DIR "${PROJECT_SOURCE_DIR}/tools/perf" CACHE PATH "Perf catalog whose benchmarks are built")

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  # -ffp-contract=off keeps results identical across compilers: no fused multiply-add reordering.
  add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow -ffp-contract=off)
endif()

if(RFMAP_COVERAGE)
  if(MSVC)
    message(FATAL_ERROR "RFMAP_COVERAGE needs GCC or Clang.")
  endif()
  # No optimisation, so every source line maps to code. Atomic counters keep the counts right in
  # the multi-threaded server and coverage code.
  add_compile_options(--coverage -O0 -g -fprofile-update=atomic)
  add_link_options(--coverage)
endif()

if(RFMAP_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
endif()

if(RFMAP_BUILD_FUZZERS)
  enable_testing()
  include(fuzz/sanitizers.cmake)
endif()

add_subdirectory(core/rf-core)
add_subdirectory(core/rf-ext)
add_subdirectory(core/rf-flightlog)
add_subdirectory(core/rf-db)
add_subdirectory(platform/rfmod)
add_subdirectory(examples/modules/body-shadow)
if(RFMAP_BUILD_SERVER)
  add_subdirectory(server)
endif()
add_subdirectory(modules/fpv)
if(RFMAP_BUILD_TESTS)
  # Modules reach stored data through host calls only: no database client in their sources.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME module_database_boundary
      COMMAND Python3::Interpreter -I ${PROJECT_SOURCE_DIR}/tools/ci/module_boundary.py --root ${PROJECT_SOURCE_DIR})
    set_tests_properties(module_database_boundary PROPERTIES LABELS modules)
  endif()
endif()
if(RFMAP_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()
# Last, so every target a benchmark links to is already defined.
if(RFMAP_BUILD_BENCHMARKS)
  add_subdirectory(tools/perf/cmake ${CMAKE_BINARY_DIR}/perf)
endif()
