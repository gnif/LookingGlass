cmake_minimum_required(VERSION 3.0)
project(lg_common_platform LANGUAGES C)

if (UNIX)
  add_subdirectory("linux")
elseif(WIN32)
  add_subdirectory("windows")
endif()

add_library(lg_common_platform INTERFACE)
target_link_libraries(lg_common_platform INTERFACE lg_common_platform_code)
