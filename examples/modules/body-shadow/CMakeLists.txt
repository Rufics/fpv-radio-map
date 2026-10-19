# Example module. Builds standalone (cmake -DRFMOD_SDK=.../platform/rfmod/include) or as part of
# the rfmap tree, where the rfmod_sdk target already exists.
cmake_minimum_required(VERSION 3.20)
project(body_shadow LANGUAGES CXX)

if(NOT TARGET rfmod_sdk)
  set(CMAKE_CXX_STANDARD 20)
  find_package(nlohmann_json 3.11 REQUIRED)
  set(RFMOD_SDK "" CACHE PATH "rfmod SDK include directory (platform/rfmod/include)")
  add_library(rfmod_sdk INTERFACE)
  target_include_directories(rfmod_sdk INTERFACE ${RFMOD_SDK})
  target_link_libraries(rfmod_sdk INTERFACE nlohmann_json::nlohmann_json)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(rfmod_sdk INTERFACE -fno-gnu-unique)
  endif()
endif()

add_library(body_shadow SHARED src/body_shadow.cpp)
target_link_libraries(body_shadow PRIVATE rfmod_sdk)
set_target_properties(body_shadow PROPERTIES CXX_VISIBILITY_PRESET hidden)

# Stage the package layout next to the library: manifest.json, schema/, lib/.
add_custom_command(TARGET body_shadow POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/stage/lib
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json ${CMAKE_CURRENT_BINARY_DIR}/stage/
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/schema ${CMAKE_CURRENT_BINARY_DIR}/stage/schema
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:body_shadow> ${CMAKE_CURRENT_BINARY_DIR}/stage/lib/)

# In the rfmap tree: pack with the public third-party dev key and run the server's upload checks.
if(TARGET rfmod AND RFMAP_BUILD_TESTS)
  get_filename_component(_keys "${CMAKE_CURRENT_SOURCE_DIR}/../../../platform/rfmod/keys" ABSOLUTE)
  add_test(NAME example_body_shadow_pack
    COMMAND $<TARGET_FILE:rfmod> pack ${CMAKE_CURRENT_BINARY_DIR}/stage --key ${_keys}/dev-thirdparty.secret
            -o ${CMAKE_CURRENT_BINARY_DIR}/example.body-shadow-1.0.0.rfmod)
  add_test(NAME example_body_shadow_test
    COMMAND $<TARGET_FILE:rfmod> test ${CMAKE_CURRENT_BINARY_DIR}/example.body-shadow-1.0.0.rfmod
            --trust ${_keys}/dev-owner.pub --trust ${_keys}/dev-thirdparty.pub)
  set_tests_properties(example_body_shadow_pack PROPERTIES LABELS example.body-shadow)
  set_tests_properties(example_body_shadow_test PROPERTIES DEPENDS example_body_shadow_pack
                       PASS_REGULAR_EXPRESSION "All checks passed" LABELS example.body-shadow)
endif()
