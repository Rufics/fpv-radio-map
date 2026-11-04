# rfmap: the command-line client of the server's HTTP API, for scripts, CI and other services.

find_package(PkgConfig REQUIRED)
# The same httplib as the server: built with TLS, so the client reaches https:// servers.
pkg_check_modules(HTTPLIB REQUIRED IMPORTED_TARGET cpp-httplib>=0.26)
find_package(OpenSSL 3 REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)

add_library(rfmap_cli_lib STATIC
  src/arguments.cpp
  src/api_client.cpp
  src/checks.cpp
  src/digest.cpp
  src/service_token.cpp
  src/cli.cpp)
target_include_directories(rfmap_cli_lib PUBLIC src)
target_link_libraries(rfmap_cli_lib PUBLIC nlohmann_json::nlohmann_json PRIVATE PkgConfig::HTTPLIB OpenSSL::SSL
  OpenSSL::Crypto)

add_executable(rfmap src/main.cpp)
target_link_libraries(rfmap PRIVATE rfmap_cli_lib)
target_compile_definitions(rfmap PRIVATE RFMAP_CLI_VERSION="${PROJECT_VERSION}")
install(TARGETS rfmap RUNTIME DESTINATION bin)

if(RFMAP_BUILD_TESTS)
  include(GoogleTest)
  add_executable(rfmap_cli_tests tests/arguments_test.cpp tests/checks_test.cpp tests/service_token_test.cpp
    tests/cli_test.cpp)
  target_link_libraries(rfmap_cli_tests PRIVATE rfmap_cli_lib PkgConfig::HTTPLIB OpenSSL::SSL OpenSSL::Crypto
    GTest::gtest_main)
  gtest_discover_tests(rfmap_cli_tests PROPERTIES LABELS cli)
endif()
