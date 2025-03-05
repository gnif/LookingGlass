cmake_minimum_required(VERSION 3.10)
project(displayserver_X11 LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(DISPLAYSERVER_X11 REQUIRED IMPORTED_TARGET
  x11
  xi
  xfixes
  xscrnsaver
  xinerama
  xcursor
  xpresent
  xkbcommon
)

add_library(displayserver_X11 STATIC
  x11.c
  atoms.c
  clipboard.c
  cursor.c

  wm/default.c
  wm/i3.c
)

add_definitions(-D GLX_GLXEXT_PROTOTYPES)

target_link_libraries(displayserver_X11
  PkgConfig::DISPLAYSERVER_X11
  lg_common
  lg_resources
)

target_include_directories(displayserver_X11
  PRIVATE
    .
)
