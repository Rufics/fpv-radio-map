include(GoogleTest)

set(_defs
  RFMAP_SCHEMAS_DIR="${PROJECT_SOURCE_DIR}/schemas"
  RFMAP_SEED_DIR="${PROJECT_SOURCE_DIR}/data/library"
  RFMAP_SQL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../sql"
  RFMOD_TEST_KEYS="${PROJECT_SOURCE_DIR}/platform/rfmod/keys"
  RFMAP_EXAMPLE_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/../config/server.example.json")

add_executable(rfmap_server_tests auth_test.cpp cache_test.cpp validation_test.cpp host_services_test.cpp
  weather_test.cpp)
target_link_libraries(rfmap_server_tests PRIVATE rfmap_server_lib GTest::gtest_main)
target_compile_definitions(rfmap_server_tests PRIVATE ${_defs})
gtest_discover_tests(rfmap_server_tests)

# Integration: a throw-away PostGIS cluster around one test process.
add_executable(rfmap_server_integration integration_test.cpp)
target_link_libraries(rfmap_server_integration PRIVATE rfmap_server_lib GTest::gtest_main)
target_compile_definitions(rfmap_server_integration PRIVATE ${_defs}
  RFMOD_WORKER="$<TARGET_FILE:rfmod-worker>"
  BODY_SHADOW_STAGE="${PROJECT_BINARY_DIR}/examples/modules/body-shadow/stage")
add_dependencies(rfmap_server_integration rfmod-worker body_shadow)

set(RFMAP_TEST_PG_PORT 55433 CACHE STRING "Port of the throw-away PostgreSQL cluster for server tests")
set(_pgdir "/tmp/rfmap-server-test-pg-${RFMAP_TEST_PG_PORT}")
add_test(NAME server_pg_start COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/pg_fixture.sh start ${_pgdir} ${RFMAP_TEST_PG_PORT})
add_test(NAME server_pg_stop COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/pg_fixture.sh stop ${_pgdir})
set_tests_properties(server_pg_start PROPERTIES FIXTURES_SETUP server_pg)
set_tests_properties(server_pg_stop PROPERTIES FIXTURES_CLEANUP server_pg)
add_test(NAME server_integration COMMAND rfmap_server_integration)
set_tests_properties(server_integration PROPERTIES
  FIXTURES_REQUIRED server_pg
  TIMEOUT 600
  ENVIRONMENT "RFMAP_TEST_DATABASE_URL=postgresql://rfmap@127.0.0.1:${RFMAP_TEST_PG_PORT}/rfmap_test")
