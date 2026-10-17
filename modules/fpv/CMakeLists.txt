# The FPV module pack. Each module is an rfmod shared library signed with the owner key, so the
# server loads it in process. They share fpv/common and use rf-core for exact geometry.

add_library(fpv_common INTERFACE)
target_include_directories(fpv_common INTERFACE common/include)
target_link_libraries(fpv_common INTERFACE rfmod_sdk rf_core)

get_filename_component(FPV_KEYS "${CMAKE_CURRENT_SOURCE_DIR}/../../platform/rfmod/keys" ABSOLUTE)

# fpv_module(<dir> <lib name> SOURCES ... [TESTS ...])
function(fpv_module dir lib)
  cmake_parse_arguments(M "" "" "SOURCES;TESTS" ${ARGN})
  set(core ${lib}_core)
  add_library(${core} OBJECT ${M_SOURCES})
  target_link_libraries(${core} PUBLIC fpv_common)
  add_library(${lib} SHARED ${dir}/src/module.cpp $<TARGET_OBJECTS:${core}>)
  target_link_libraries(${lib} PRIVATE fpv_common)
  set_target_properties(${lib} PROPERTIES CXX_VISIBILITY_PRESET hidden PREFIX "lib")
  set(stage ${CMAKE_CURRENT_BINARY_DIR}/${dir}/stage)
  add_custom_command(TARGET ${lib} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${stage}/lib
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/manifest.json ${stage}/
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/schema ${stage}/schema
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${lib}> ${stage}/lib/)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/ui)
    add_custom_command(TARGET ${lib} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/ui ${stage}/ui)
  endif()
  if(RFMAP_BUILD_TESTS)
    set(pkg ${CMAKE_CURRENT_BINARY_DIR}/${dir}.rfmod)
    add_test(NAME ${lib}_pack COMMAND $<TARGET_FILE:rfmod> pack ${stage} --key ${FPV_KEYS}/dev-owner.secret -o ${pkg})
    add_test(NAME ${lib}_upload_checks COMMAND $<TARGET_FILE:rfmod> test ${pkg} --trust ${FPV_KEYS}/dev-owner.pub)
    set_tests_properties(${lib}_upload_checks PROPERTIES DEPENDS ${lib}_pack PASS_REGULAR_EXPRESSION "All checks passed")
    if(M_TESTS)
      add_executable(${lib}_tests ${M_TESTS} $<TARGET_OBJECTS:${core}>)
      target_include_directories(${lib}_tests PRIVATE ${dir}/src)
      target_link_libraries(${lib}_tests PRIVATE fpv_common GTest::gtest_main)
      include(GoogleTest)
      gtest_discover_tests(${lib}_tests)
    endif()
  endif()
endfunction()

fpv_module(battery fpv_battery
  SOURCES battery/src/energy_model.cpp battery/src/analysis.cpp
  TESTS battery/tests/energy_model_test.cpp)

fpv_module(min-altitude fpv_min_altitude
  SOURCES min-altitude/src/clearance.cpp min-altitude/src/analysis.cpp
  TESTS min-altitude/tests/clearance_test.cpp)
