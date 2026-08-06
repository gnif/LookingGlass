cmake_minimum_required(VERSION 3.10)

add_executable(font-tests
  font_test.c
  ../src/font.c
)
target_compile_definitions(font-tests PRIVATE
  FONT_TEST_FILE="${PROJECT_TOP}/repos/gui/cimgui/imgui/misc/fonts/DroidSans.ttf"
)
target_include_directories(font-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../include"
)
add_test(NAME font-tests
  COMMAND font-tests
)
set_tests_properties(font-tests PROPERTIES
  TIMEOUT 10
)

add_executable(lgmp-transport-tests
  lgmp_transport_test.c
)
target_link_libraries(lgmp-transport-tests
  ${EXE_FLAGS}
  transport_LGMP
  lg_common
  lgmp
)
add_test(NAME lgmp-transport-tests
  COMMAND lgmp-transport-tests
)
set_tests_properties(lgmp-transport-tests PROPERTIES
  TIMEOUT 10
)

add_executable(wayland-motion-tests
  motion_test.c
)
target_include_directories(wayland-motion-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../displayservers/Wayland"
)
add_test(NAME wayland-motion-tests
  COMMAND wayland-motion-tests
)
set_tests_properties(wayland-motion-tests PROPERTIES
  TIMEOUT 10
)

add_executable(mouse-tests
  mouse_test.c
  ../src/core.c
  ../src/util.c
)
target_compile_definitions(mouse-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(mouse-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/PureSpice/include"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(mouse-tests
  ${EXE_FLAGS}
  lg_common
)
set(MOUSE_CASES
  inset-exit
  surface-exit
  exit-delay
  exit-guest
  exit-cancel
  confine-pending
  confine-guest
  capture-pending
  capture-revoked
  capture-release
  capture-fallback
  rotate-scale
  geometry
  edges
  scales
  border-exit
)
foreach(name IN LISTS MOUSE_CASES)
  add_test(NAME mouse-${name}
    COMMAND mouse-tests ${name}
  )
  set_tests_properties(mouse-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

find_package(GTest REQUIRED)
find_program(WESTON_EXECUTABLE NAMES weston)
if(NOT WESTON_EXECUTABLE)
  message(FATAL_ERROR "weston is required to run framebuffer rendering tests")
endif()

add_executable(render-tests
  render_test.cpp
)
add_dependencies(render-tests looking-glass-client)

set_target_properties(render-tests PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)

target_compile_options(render-tests PRIVATE
  -fno-fast-math
)

target_compile_definitions(render-tests PRIVATE
  LG_CLIENT_PATH="$<TARGET_FILE:looking-glass-client>"
)

target_include_directories(render-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../include"
  "${PROJECT_TOP}/common/include"
)

if(TARGET GTest::gtest_main)
  target_link_libraries(render-tests GTest::gtest_main)
else()
  target_link_libraries(render-tests GTest::Main)
endif()

add_test(NAME render-tests
  COMMAND /bin/bash
    "${CMAKE_CURRENT_SOURCE_DIR}/run-with-weston.sh"
    "${WESTON_EXECUTABLE}"
    "$<TARGET_FILE:render-tests>"
)

set_tests_properties(render-tests PROPERTIES
  TIMEOUT 120
)
