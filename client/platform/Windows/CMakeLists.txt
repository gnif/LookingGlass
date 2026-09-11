cmake_minimum_required(VERSION 3.10)
project(client_platform_Windows LANGUAGES C)

add_library(client_platform_Windows STATIC
  clipboard_files.c
  config.c
  font.c
  input.c
  process.c
)

target_link_libraries(client_platform_Windows
  PUBLIC
    lg_common
    dwrite
    ole32
    shell32
    shlwapi
    uuid
)

target_include_directories(client_platform_Windows
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)
