# rfmap-server: HTTP API over rf-core, rf-ext and the rfmod module platform.

find_package(PkgConfig REQUIRED)
pkg_check_modules(PQXX REQUIRED IMPORTED_TARGET libpqxx)
pkg_check_modules(HTTPLIB REQUIRED IMPORTED_TARGET cpp-httplib)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
find_package(GDAL CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)

add_library(rfmap_server_lib STATIC
  src/command_line.cpp
  src/common.cpp
  src/security.cpp
  src/config.cpp
  src/database.cpp
  src/auth.cpp
  src/pg_auth_store.cpp
  src/schemas.cpp
  src/library.cpp
  src/missions.cpp
  src/builtins.cpp
  src/raster.cpp
  src/result_cache.cpp
  src/weather.cpp
  src/link_service.cpp
  src/module_runner.cpp
  src/host_services.cpp
  src/calculator.cpp
  src/rate_limiter.cpp
  src/jobs.cpp
  src/metrics.cpp
  src/logger.cpp
  src/app.cpp)
target_include_directories(rfmap_server_lib PUBLIC src)
target_link_libraries(rfmap_server_lib
  PUBLIC rf_ext rfmod_platform nlohmann_json::nlohmann_json PkgConfig::PQXX PkgConfig::SODIUM
         PkgConfig::HTTPLIB GDAL::GDAL Threads::Threads
  PRIVATE nlohmann_json_schema_validator)
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
