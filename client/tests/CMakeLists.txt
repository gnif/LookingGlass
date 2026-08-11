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
  ../src/kb_hid.c
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

add_executable(lgmp-frame-tests
  lgmp_frame_test.c
  ../src/kb_hid.c
)
target_link_libraries(lgmp-frame-tests
  ${EXE_FLAGS}
  transport_LGMP
  lg_common
  lgmp
)
set(LGMP_FRAME_CASES
  malformed
  serial-damage
  lanes
  leases
  dma
)
foreach(name IN LISTS LGMP_FRAME_CASES)
  add_test(NAME lgmp-frame-${name}
    COMMAND lgmp-frame-tests ${name}
  )
  set_tests_properties(lgmp-frame-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

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

add_executable(displayserver-input-tests
  displayserver_input_test.c
  ../displayservers/Wayland/input_event.c
  ../displayservers/X11/input_event.c
)
target_include_directories(displayserver-input-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../displayservers"
)

set(DISPLAYSERVER_INPUT_CASES
  pointer-events
  pointer-buttons
  pointer-release
  wheel-steps
  relative-motion
  keyboard-focus
  keyboard-keys
  keyboard-state
)

foreach(backend IN ITEMS wayland x11)
  foreach(name IN LISTS DISPLAYSERVER_INPUT_CASES)
    add_test(NAME display-input-${backend}-${name}
      COMMAND displayserver-input-tests ${backend} ${name}
    )
    set_tests_properties(display-input-${backend}-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()
endforeach()

foreach(name IN ITEMS wheel-residual wheel-multiple)
  add_test(NAME display-input-wayland-${name}
    COMMAND displayserver-input-tests wayland ${name}
  )
  set_tests_properties(display-input-wayland-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

foreach(name IN ITEMS pointer-routing keyboard-routing held-keys)
  add_test(NAME display-input-x11-${name}
    COMMAND displayserver-input-tests x11 ${name}
  )
  set_tests_properties(display-input-x11-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(mouse-tests
  mouse_test.c
  ../src/core.c
  ../src/input.c
  ../src/util.c
)
target_compile_definitions(mouse-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(mouse-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
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
  exit-immediate
  exit-guest
  absolute-sync
  auto-capture
  exit-reentry
  view-immediate
  capture-pending
  capture-revoked
  capture-release
  capture-align
  x11-capture-exit
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

add_executable(input-tests
  input_test.c
  ../src/input.c
)
target_include_directories(input-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(input-tests
  ${EXE_FLAGS}
  lg_common
)
set(INPUT_CASES
  preference
  handoff
  stale
  dead-drop
  rollback
  duplicate
  absolute
)
foreach(name IN LISTS INPUT_CASES)
  add_test(NAME input-${name}
    COMMAND input-tests ${name}
  )
  set_tests_properties(input-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(clipboard-tests
  clipboard_test.c
  ../src/clipboard.c
)
target_compile_definitions(clipboard-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(clipboard-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(clipboard-tests
  ${EXE_FLAGS}
  lg_common
)
set(CLIPBOARD_CASES
  preference
  request
  invalid
  cancel
  generation
  local
  reentrant
)
foreach(name IN LISTS CLIPBOARD_CASES)
  add_test(NAME clipboard-${name}
    COMMAND clipboard-tests ${name}
  )
  set_tests_properties(clipboard-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(transport-fallback-tests
  transport_fallback_test.c
  ../src/transport_fallback.c
)
target_include_directories(transport-fallback-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(transport-fallback-tests
  ${EXE_FLAGS}
  lg_common
)
if(ENABLE_AUDIO)
  target_compile_definitions(transport-fallback-tests PRIVATE
    ENABLE_AUDIO
  )
endif()
set(TRANSPORT_FALLBACK_CASES
  graceful
  dead
  mismatch
  revoke
  retry
  cancel
)
foreach(name IN LISTS TRANSPORT_FALLBACK_CASES)
  add_test(NAME transport-fallback-${name}
    COMMAND transport-fallback-tests ${name}
  )
  set_tests_properties(transport-fallback-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(frame-scheduler-tests
  frame_scheduler_test.c
  ../src/frame_scheduler.c
)
target_compile_definitions(frame-scheduler-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(frame-scheduler-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(frame-scheduler-tests
  ${EXE_FLAGS}
  lg_common
)
set(FRAME_SCHEDULER_CASES
  absent
  start
  enqueue
  ack
  period
  cadence
  feedback
)
foreach(name IN LISTS FRAME_SCHEDULER_CASES)
  add_test(NAME frame-scheduler-${name}
    COMMAND frame-scheduler-tests ${name}
  )
  set_tests_properties(frame-scheduler-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

if(ENABLE_AUDIO)
  pkg_check_modules(AUDIO_TEST_SAMPLERATE REQUIRED IMPORTED_TARGET
    samplerate)
  add_executable(audio-tests
    audio_test.c
  )
  target_compile_definitions(audio-tests PRIVATE
    CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
    ENABLE_AUDIO
  )
  target_include_directories(audio-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${PROJECT_TOP}/repos/gui/cimgui"
    "${PROJECT_TOP}/repos/gui/cimgui/imgui"
  )
  target_link_libraries(audio-tests
    ${EXE_FLAGS}
    lg_common
    PkgConfig::AUDIO_TEST_SAMPLERATE
  )
  set(AUDIO_CASES
    format
    convert
    provider
    playback-retry
    consent
    quiesce
  )
  foreach(name IN LISTS AUDIO_CASES)
    add_test(NAME audio-${name}
      COMMAND audio-tests ${name}
    )
    set_tests_properties(audio-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()
endif()

add_executable(render-queue-tests
  render_queue_test.c
  ../src/render_queue.c
)
target_compile_definitions(render-queue-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(render-queue-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(render-queue-tests
  ${EXE_FLAGS}
  lg_common
)
set(RENDER_QUEUE_CASES
  generation
  latest
  prepare
  presented
  payload
  validation
  cache
)
foreach(name IN LISTS RENDER_QUEUE_CASES)
  add_test(NAME render-queue-${name}
    COMMAND render-queue-tests ${name}
  )
  set_tests_properties(render-queue-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

if(ENABLE_EGL)
  add_executable(desktop-rects-tests
    desktop_rects_test.c
  )
  target_include_directories(desktop-rects-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../renderers/EGL"
  )
  target_link_libraries(desktop-rects-tests
    ${EXE_FLAGS}
    renderer_EGL
  )
  set(DESKTOP_RECTS_CASES
    rotations
    fractional
    round-trip
    clipping
  )
  foreach(name IN LISTS DESKTOP_RECTS_CASES)
    add_test(NAME desktop-rects-${name}
      COMMAND desktop-rects-tests ${name}
    )
    set_tests_properties(desktop-rects-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()
endif()

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
