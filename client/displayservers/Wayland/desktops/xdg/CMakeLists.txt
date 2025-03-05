cmake_minimum_required(VERSION 3.10)
project(wayland_desktop_xdg LANGUAGES C)

add_library(wayland_desktop_xdg STATIC
  xdg.c
)

target_link_libraries(wayland_desktop_xdg
  lg_common
  wayland_protocol
)

include_directories(
  "../../"
)
