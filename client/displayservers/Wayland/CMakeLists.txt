cmake_minimum_required(VERSION 3.5)
project(displayserver_Wayland LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(DISPLAYSERVER_Wayland REQUIRED IMPORTED_TARGET
  wayland-client
  wayland-cursor
  xkbcommon
)

find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner)

add_library(displayserver_Wayland STATIC
  activation.c
  clipboard.c
  cursor.c
  gl.c
  idle.c
  input.c
  output.c
  poll.c
  presentation.c
  state.c
  registry.c
  vulkan.c
  wayland.c
  window.c
)

add_subdirectory(protocol)
add_subdirectory(desktops)

target_link_libraries(displayserver_Wayland
  PkgConfig::DISPLAYSERVER_Wayland
  lg_common
  wayland_protocol
  wayland_desktops
)

target_include_directories(displayserver_Wayland
  PRIVATE
    .
)
