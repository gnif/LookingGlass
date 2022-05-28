cmake_minimum_required(VERSION 3.0)
project(platform_Windows LANGUAGES C)

include_directories(
  ${PROJECT_SOURCE_DIR}/include
)

add_library(platform_Windows STATIC
  src/platform.c
  src/service.c
  src/mousehook.c
  src/force_compose.c
)

# allow use of functions for Windows 7 or later
add_compile_definitions(WINVER=0x0601 _WIN32_WINNT=0x0601)

add_subdirectory("capture")

target_link_libraries(platform_Windows
  lg_common
  capture

  userenv
  wtsapi32
  psapi
  shlwapi
  powrprof
  rpcrt4
  avrt
)

target_include_directories(platform_Windows
  PRIVATE
    src
)

# these are for the nsis installer generator
configure_file("${PROJECT_SOURCE_DIR}/installer.nsi" "${PROJECT_BINARY_DIR}/installer.nsi" COPYONLY)
configure_file("${PROJECT_TOP}/resources/icon.ico"   "${PROJECT_BINARY_DIR}/icon.ico"      COPYONLY)
configure_file("${PROJECT_TOP}/LICENSE"              "${PROJECT_BINARY_DIR}/LICENSE.txt"   COPYONLY)
