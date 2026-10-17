# rf-ext: optional RF extensions on top of rf-core. Reusable by any program that uses rf-core.
add_library(rf_ext
  src/refractivity.cpp
  src/rain.cpp
  src/clutter.cpp
  src/uncertainty.cpp)
add_library(rfmap::rf_ext ALIAS rf_ext)
target_include_directories(rf_ext PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(rf_ext PUBLIC rf_core)

if(RFMAP_BUILD_TESTS)
  add_executable(rf_ext_tests
    tests/refractivity_test.cpp
    tests/rain_test.cpp
    tests/clutter_test.cpp
    tests/uncertainty_test.cpp)
  target_include_directories(rf_ext_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../rf-core/tests)
  target_link_libraries(rf_ext_tests PRIVATE rf_ext GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(rf_ext_tests)
endif()
