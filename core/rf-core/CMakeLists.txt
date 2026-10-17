find_package(GeographicLib REQUIRED)

add_library(rf_core
  src/geodesy.cpp
  src/earth_model.cpp
  src/fresnel_integrals.cpp
  src/propagation.cpp
  src/path_geometry.cpp
  src/terrain_profile.cpp
  src/diffraction.cpp
  src/receiver.cpp
  src/link_budget.cpp
)
add_library(rfmap::rf_core ALIAS rf_core)
target_include_directories(rf_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(rf_core PUBLIC ${GeographicLib_LIBRARIES})
target_include_directories(rf_core PUBLIC ${GeographicLib_INCLUDE_DIRS})

if(RFMAP_BUILD_TESTS)
  add_executable(rf_core_tests
    tests/units_test.cpp
    tests/fresnel_integrals_test.cpp
    tests/propagation_test.cpp
    tests/geodesy_test.cpp
    tests/diffraction_test.cpp
    tests/link_budget_test.cpp
    tests/reference_case_test.cpp
  )
  target_link_libraries(rf_core_tests PRIVATE rf_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(rf_core_tests)
endif()
