cmake_minimum_required(VERSION 3.0)
project(platform_Linux LANGUAGES C)

include_directories(
  ${PROJECT_SOURCE_DIR}/include
)

add_library(platform_Linux STATIC
  src/platform.c
)

add_subdirectory("capture")

target_link_libraries(platform_Linux
  m
  capture
  pthread
)

target_include_directories(platform_Linux
  PRIVATE
    src
)
