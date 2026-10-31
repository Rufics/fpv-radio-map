# rf-db/pg: rf-db for PostgreSQL over libpqxx. Built with the server, the only program that
# talks to the database; modules reach it through host calls, never through this library.
find_package(PkgConfig REQUIRED)
# The shared libpqxx 7.10 of Ubuntu 26.04 and the program using it both destroy the same
# pqxx::type_name strings at exit, a double free that aborts every run: link the static archive.
pkg_check_modules(PQXX REQUIRED libpqxx>=7.10)
pkg_check_modules(PQ REQUIRED IMPORTED_TARGET libpq)
find_library(PQXX_ARCHIVE NAMES libpqxx.a HINTS ${PQXX_LIBRARY_DIRS} REQUIRED)
add_library(rfmap_pqxx INTERFACE)
target_include_directories(rfmap_pqxx SYSTEM INTERFACE ${PQXX_INCLUDE_DIRS})
target_link_libraries(rfmap_pqxx INTERFACE ${PQXX_ARCHIVE} PkgConfig::PQ)

add_library(rf_db_pg
  src/connection_string.cpp
  src/session.cpp
  src/session_pool.cpp)
add_library(rfmap::rf_db_pg ALIAS rf_db_pg)
target_include_directories(rf_db_pg PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(rf_db_pg PUBLIC rf_db rfmap_pqxx)

if(RFMAP_BUILD_TESTS)
  add_executable(rf_db_pg_tests
    tests/connection_string_test.cpp)
  target_link_libraries(rf_db_pg_tests PRIVATE rf_db_pg GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(rf_db_pg_tests PROPERTIES LABELS rf-db)
endif()
