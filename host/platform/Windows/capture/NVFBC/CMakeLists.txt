cmake_minimum_required(VERSION 3.0)
project(capture_NVFBC LANGUAGES C CXX)

add_library(capture_NVFBC STATIC
  src/nvfbc.c
  src/wrapper.cpp
)

file(TO_CMAKE_PATH "${NVFBC_SDK}" nvfbc_sdk)
include_directories(file, "${nvfbc_sdk}/inc")

target_link_libraries(capture_NVFBC
  platform_Windows
  dwmapi
  d3d9
)

target_include_directories(capture_NVFBC
  PRIVATE
    src
)
