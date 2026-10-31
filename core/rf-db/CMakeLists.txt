# rf-db: the database layer every component of the server goes through. It knows nothing about a
# particular database: ConnectionPool lends connections a ConnectionFactory opens, bounded and
# health-checked; rf-db-pg implements the two interfaces for PostgreSQL.
find_package(Threads REQUIRED)

add_library(rf_db
  src/connection_pool.cpp)
add_library(rfmap::rf_db ALIAS rf_db)
target_include_directories(rf_db PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(rf_db PUBLIC Threads::Threads)

if(RFMAP_BUILD_TESTS)
  add_executable(rf_db_tests
    tests/connection_pool_test.cpp)
  target_link_libraries(rf_db_tests PRIVATE rf_db GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(rf_db_tests PROPERTIES LABELS rf-db)
endif()
