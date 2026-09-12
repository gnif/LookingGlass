cmake_minimum_required(VERSION 3.10)
project(client_platform_Linux LANGUAGES C)

set(CLIENT_PLATFORM_LINUX_SOURCES
  ../../src/clipboard_files.c
  ../../src/event_source.c
  ../../src/evdev.c
  config.c
  font.c
  input.c
  process.c
)

add_library(client_platform_Linux STATIC ${CLIENT_PLATFORM_LINUX_SOURCES})

target_link_libraries(client_platform_Linux
  PUBLIC
    lg_common
    PkgConfig::FONTCONFIG
    PkgConfig::FUSE3
)

target_include_directories(client_platform_Linux
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)
