cmake_minimum_required(VERSION 3.10)
project(client_platform_Linux LANGUAGES C)

set(CLIENT_PLATFORM_LINUX_SOURCES
  ../../src/clipboard_files.c
  ../../src/evdev.c
)

foreach(source IN ITEMS config.c font.c input.c process.c)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    list(APPEND CLIENT_PLATFORM_LINUX_SOURCES ${source})
  endif()
endforeach()

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
