# rfmap_perf_benchmark(): the benchmark binary of the catalog item in the calling folder.
#
# The target name and the libraries come from the item's manifest.json ("target", "links"), so
# the manifest stays the one place that says them. Every *.cpp in the folder is a source; the
# binary gets benchmark_main, so the sources only register benchmarks.
function(rfmap_perf_benchmark)
  set(_manifest "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_manifest}")
  file(READ "${_manifest}" _json)
  string(JSON _target GET "${_json}" target)
  string(JSON _count LENGTH "${_json}" links)
  set(_links "")
  math(EXPR _last "${_count} - 1")
  foreach(_index RANGE ${_last})
    string(JSON _link GET "${_json}" links ${_index})
    if(NOT TARGET ${_link})
      message(FATAL_ERROR "${_manifest}: links names '${_link}', which is not a target of this build "
                          "(the server libraries need RFMAP_BUILD_SERVER=ON)")
    endif()
    list(APPEND _links ${_link})
  endforeach()
  file(GLOB _sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
  add_executable(${_target} ${_sources})
  target_link_libraries(${_target} PRIVATE ${_links} benchmark::benchmark benchmark::benchmark_main)
  set_target_properties(${_target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${RFMAP_PERF_BIN_DIR}")
endfunction()
