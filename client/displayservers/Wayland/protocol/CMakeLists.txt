cmake_minimum_required(VERSION 3.10)
project(wayland_protocol LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET
  wayland-client
  wayland-cursor
  xkbcommon
)

find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner)

add_library(wayland_protocol STATIC
)

macro(wayland_generate protocol_file output_file)
  add_custom_command(OUTPUT "${output_file}.h"
    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${protocol_file}" "${output_file}.h"
    DEPENDS "${protocol_file}"
    VERBATIM)

  add_custom_command(OUTPUT "${output_file}.c"
    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${protocol_file}" "${output_file}.c"
    DEPENDS "${protocol_file}"
    VERBATIM)

  target_sources(wayland_protocol PRIVATE "${output_file}.h" "${output_file}.c")
endmacro()

set(WAYLAND_PROTOCOLS_BASE "${PROJECT_TOP}/repos/wayland-protocols")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/wayland")
include_directories("${CMAKE_BINARY_DIR}/wayland")
wayland_generate(
  "${WAYLAND_PROTOCOLS_BASE}/stable/xdg-shell/xdg-shell.xml"
  "${CMAKE_BINARY_DIR}/wayland/wayland-xdg-shell-client-protocol")
wayland_generate(
  "${WAYLAND_PROTOCOLS_BASE}/stable/presentation-time/presentation-time.xml"
  "${CMAKE_BINARY_DIR}/wayland/wayland-presentation-time-client-protocol")
wayland_generate(
  "${WAYLAND_PROTOCOLS_BASE}/stable/viewporter/viewporter.xml"
  "${CMAKE_BINARY_DIR}/wayland/wayland-viewporter-client-protocol")
wayland_generate(
  "${WAYLAND_PROTOCOLS_BASE}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
  "${CMAKE_BINARY_DIR}/wayland/wayland-xdg-decoration-unstable-v1-client-protocol")
wayland_generate(
  "${WAYLAND_PROTOCOLS_BASE}/unstable/relative-pointer/relative-pointer-unstable-v1.xml"
  "${CMAKE_BINARY_DIR}/wayland/wayland-relative-pointer-unstable-v1-client-protocol")
wayland_generate(
  "${WAYLAND_PROTOCOLS_BASE}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml"
  "${CMAKE_BINARY_DIR}/wayland/wayland-pointer-constraints-unstable-v1-client-protocol")
wayland_generate(
  "${WAYLAND_PROTOCOLS_BASE}/unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml"
  "${CMAKE_BINARY_DIR}/wayland/wayland-keyboard-shortcuts-inhibit-unstable-v1-client-protocol")
wayland_generate(
  "${WAYLAND_PROTOCOLS_BASE}/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml"
  "${CMAKE_BINARY_DIR}/wayland/wayland-idle-inhibit-unstable-v1-client-protocol")
wayland_generate(
  "${WAYLAND_PROTOCOLS_BASE}/unstable/xdg-output/xdg-output-unstable-v1.xml"
  "${CMAKE_BINARY_DIR}/wayland/wayland-xdg-output-unstable-v1-client-protocol")
wayland_generate(
  "${WAYLAND_PROTOCOLS_BASE}/staging/xdg-activation/xdg-activation-v1.xml"
  "${CMAKE_BINARY_DIR}/wayland/wayland-xdg-activation-v1-client-protocol")

target_link_libraries(wayland_protocol
  PkgConfig::WAYLAND
)

target_include_directories(wayland_protocol
  PUBLIC
    "${CMAKE_BINARY_DIR}/wayland"
)
