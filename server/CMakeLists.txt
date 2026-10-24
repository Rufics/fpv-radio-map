# rfmap-server: HTTP API over rf-core, rf-ext and the rfmod module platform.

find_package(PkgConfig REQUIRED)
# The shared libpqxx 7.10 of Ubuntu 26.04 and the program using it both destroy the same
# pqxx::type_name strings at exit, a double free that aborts every run: link the static archive.
pkg_check_modules(PQXX REQUIRED libpqxx>=7.10)
pkg_check_modules(PQ REQUIRED IMPORTED_TARGET libpq)
find_library(PQXX_ARCHIVE NAMES libpqxx.a HINTS ${PQXX_LIBRARY_DIRS} REQUIRED)
add_library(rfmap_pqxx INTERFACE)
target_include_directories(rfmap_pqxx SYSTEM INTERFACE ${PQXX_INCLUDE_DIRS})
target_link_libraries(rfmap_pqxx INTERFACE ${PQXX_ARCHIVE} PkgConfig::PQ)
# httplib 0.26 is built with TLS, zlib and Brotli support and its header calls those libraries
# inline, but its pkg-config file lists them for static linking only: link them here.
pkg_check_modules(HTTPLIB REQUIRED IMPORTED_TARGET cpp-httplib>=0.26)
find_package(OpenSSL 3 REQUIRED)
find_package(LibArchive REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
find_package(GDAL CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)

add_library(rfmap_server_lib STATIC
  src/command_line.cpp
  src/common.cpp
  src/security.cpp
  src/pin_gate.cpp
  src/config.cpp
  src/database.cpp
  src/auth.cpp
  src/pg_auth_store.cpp
  src/schemas.cpp
  src/library.cpp
  src/missions.cpp
  src/flight_logs.cpp
  src/builtins.cpp
  src/raster.cpp
  src/result_cache.cpp
  src/weather.cpp
  src/tiles_manifest.cpp
  src/link_service.cpp
  src/module_runner.cpp
  src/invocation_scope.cpp
  src/host_methods.cpp
  src/host_services.cpp
  src/grid_outline.cpp
  src/result_key.cpp
  src/route_altitude.cpp
  src/route_evaluator.cpp
  src/coverage_engine.cpp
  src/calculator.cpp
  src/rate_limiter.cpp
  src/jobs.cpp
  src/metrics.cpp
  src/logger.cpp
  src/request.cpp
  src/cache_admin.cpp
  src/map_config.cpp
  src/module_catalog.cpp
  src/services.cpp
  src/mission_api.cpp
  src/flight_log_api.cpp
  src/data_api.cpp
  src/admin_api.cpp
  src/migrations.cpp
  src/staged_files.cpp
  src/backup.cpp
  src/backup_stream.cpp
  src/file_lock.cpp
  src/app.cpp)
target_include_directories(rfmap_server_lib PUBLIC src)
target_link_libraries(rfmap_server_lib
  PUBLIC rf_ext rf_flightlog rfmod_platform nlohmann_json::nlohmann_json rfmap_pqxx PkgConfig::SODIUM
         PkgConfig::HTTPLIB OpenSSL::SSL OpenSSL::Crypto GDAL::GDAL Threads::Threads
  PRIVATE nlohmann_json_schema_validator LibArchive::LibArchive)
# GDAL, libpqxx and httplib headers are third-party: include them as system headers so our
# warning flags apply to our code only.
get_target_property(_gdal_inc GDAL::GDAL INTERFACE_INCLUDE_DIRECTORIES)
if(_gdal_inc)
  target_include_directories(rfmap_server_lib SYSTEM PUBLIC ${_gdal_inc})
endif()

add_executable(rfmap-server src/main.cpp)
target_link_libraries(rfmap-server PRIVATE rfmap_server_lib)
target_compile_definitions(rfmap-server PRIVATE RFMAP_DEFAULT_SQL_DIR="${CMAKE_INSTALL_PREFIX}/share/rfmap/sql")
add_dependencies(rfmap-server rfmod-worker)

install(TARGETS rfmap-server RUNTIME DESTINATION bin)
install(TARGETS rfmod-worker RUNTIME DESTINATION lib/rfmap)
install(DIRECTORY sql/ DESTINATION share/rfmap/sql)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/schemas/ DESTINATION share/rfmap/schemas)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/data/library/ DESTINATION share/rfmap/library)

if(RFMAP_BUILD_TESTS)
  add_subdirectory(tests)
endif()
