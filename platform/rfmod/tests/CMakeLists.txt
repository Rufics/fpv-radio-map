function(rfmod_test_module target id version selftest_fails)
  add_library(${target} SHARED modules/test_module.cpp)
  target_link_libraries(${target} PRIVATE rfmod_sdk)
  target_compile_definitions(${target} PRIVATE TEST_ID="${id}" TEST_VERSION="${version}" SELFTEST_FAILS=${selftest_fails})
  set_target_properties(${target} PROPERTIES CXX_VISIBILITY_PRESET hidden)
endfunction()

rfmod_test_module(rfmod_test_alpha test.alpha 1.0.0 0)
rfmod_test_module(rfmod_test_alpha2 test.alpha 1.1.0 0)
rfmod_test_module(rfmod_test_beta test.beta 0.2.0 0)
rfmod_test_module(rfmod_test_broken test.broken 1.0.0 1)

add_executable(rfmod_tests
  semver_test.cpp
  package_test.cpp
  manifest_test.cpp
  registry_test.cpp)
target_link_libraries(rfmod_tests PRIVATE rfmod_platform GTest::gtest_main)
add_dependencies(rfmod_tests rfmod-worker rfmod_test_alpha rfmod_test_alpha2 rfmod_test_beta rfmod_test_broken)
target_compile_definitions(rfmod_tests PRIVATE
  RFMOD_TEST_KEYS="${CMAKE_CURRENT_SOURCE_DIR}/../keys"
  RFMOD_WORKER="$<TARGET_FILE:rfmod-worker>"
  RFMOD_LIB_ALPHA="$<TARGET_FILE:rfmod_test_alpha>"
  RFMOD_LIB_ALPHA2="$<TARGET_FILE:rfmod_test_alpha2>"
  RFMOD_LIB_BETA="$<TARGET_FILE:rfmod_test_beta>"
  RFMOD_LIB_BROKEN="$<TARGET_FILE:rfmod_test_broken>")
include(GoogleTest)
gtest_discover_tests(rfmod_tests)
