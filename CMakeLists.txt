cmake_minimum_required(VERSION 3.25)
project(fpv_radio_map VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

list(APPEND CMAKE_MODULE_PATH /usr/share/cmake/geographiclib)

option(RFMAP_BUILD_TESTS "Build unit tests" ON)
option(RFMAP_BUILD_SERVER "Build the HTTP server" ON)

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  # -ffp-contract=off keeps results identical across compilers: no fused multiply-add reordering.
  add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow -ffp-contract=off)
endif()

if(RFMAP_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
endif()

add_subdirectory(core/rf-core)
add_subdirectory(core/rf-ext)
add_subdirectory(platform/rfmod)
add_subdirectory(examples/modules/body-shadow)
add_subdirectory(modules/fpv)
