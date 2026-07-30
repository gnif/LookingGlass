cmake_minimum_required(VERSION 3.10)

find_package(GTest REQUIRED)
find_program(WESTON_EXECUTABLE NAMES weston)
if(NOT WESTON_EXECUTABLE)
  message(FATAL_ERROR "weston is required to run framebuffer rendering tests")
endif()

add_executable(looking-glass-render-tests
  render_test.cpp
)
add_dependencies(looking-glass-render-tests looking-glass-client)

set_target_properties(looking-glass-render-tests PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)

target_compile_definitions(looking-glass-render-tests PRIVATE
  LG_CLIENT_PATH="$<TARGET_FILE:looking-glass-client>"
)

target_include_directories(looking-glass-render-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../include"
  "${PROJECT_TOP}/common/include"
)

if(TARGET GTest::gtest_main)
  target_link_libraries(looking-glass-render-tests GTest::gtest_main)
else()
  target_link_libraries(looking-glass-render-tests GTest::Main)
endif()

add_test(NAME looking-glass-render-tests
  COMMAND /bin/bash
    "${CMAKE_CURRENT_SOURCE_DIR}/run-with-weston.sh"
    "${WESTON_EXECUTABLE}"
    "$<TARGET_FILE:looking-glass-render-tests>"
)

set_tests_properties(looking-glass-render-tests PROPERTIES
  TIMEOUT 120
)
