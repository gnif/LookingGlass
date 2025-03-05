cmake_minimum_required(VERSION 3.10)
project(wayland_desktop_libdecor LANGUAGES C)

add_library(wayland_desktop_libdecor STATIC
  libdecor.c
)

target_link_libraries(wayland_desktop_libdecor
  lg_common
  wayland_protocol
  PkgConfig::LIBDECOR
)

include_directories(
  "../../"
)
