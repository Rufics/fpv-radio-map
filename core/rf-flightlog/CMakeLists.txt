# rf-flightlog: turns a normalized flight log (schemas/flightlog.schema.json), whichever decoder
# module produced it, into the replay view the map draws: an even timeline, energy, mode bands,
# the story of the flight and the plan-against-fact comparison.
find_package(nlohmann_json 3.11 REQUIRED)

add_library(rf_flightlog
  src/flight_log.cpp
  src/timeline.cpp
  src/story.cpp
  src/plan_comparison.cpp
  src/processor.cpp)
add_library(rfmap::rf_flightlog ALIAS rf_flightlog)
target_include_directories(rf_flightlog PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(rf_flightlog PUBLIC rf_core nlohmann_json::nlohmann_json)

if(RFMAP_BUILD_TESTS)
  add_executable(rf_flightlog_tests
    tests/timeline_test.cpp
    tests/story_test.cpp
    tests/plan_comparison_test.cpp
    tests/processor_test.cpp)
  target_link_libraries(rf_flightlog_tests PRIVATE rf_flightlog GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(rf_flightlog_tests PROPERTIES LABELS rf-flightlog)
endif()
