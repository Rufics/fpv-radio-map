# Builds every benchmark of the perf catalog: each folder in extensions/benchmarks with a
# CMakeLists.txt. The binaries land in <build>/perf/bin, where perf.py looks for them.
find_package(benchmark REQUIRED)
include(${CMAKE_CURRENT_LIST_DIR}/PerfBenchmark.cmake)

set(RFMAP_PERF_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/bin")
file(GLOB _perf_items LIST_DIRECTORIES true "${RFMAP_PERF_CATALOG_DIR}/extensions/benchmarks/*")
list(SORT _perf_items)
foreach(_perf_item IN LISTS _perf_items)
  if(EXISTS "${_perf_item}/CMakeLists.txt")
    get_filename_component(_perf_id "${_perf_item}" NAME)
    add_subdirectory("${_perf_item}" "${CMAKE_CURRENT_BINARY_DIR}/${_perf_id}")
  endif()
endforeach()
