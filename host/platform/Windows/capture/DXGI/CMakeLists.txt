cmake_minimum_required(VERSION 3.0)
project(capture_DXGI LANGUAGES C)

add_library(capture_DXGI STATIC
  src/dxgi.c
  src/d3d11.c
  src/d3d12.c
  src/ods_capture.c
  src/util.c
)

add_definitions("-DCOBJMACROS -DINITGUID")

target_link_libraries(capture_DXGI
  lg_common
  d3d11
  dxgi
  dwmapi
)

target_include_directories(capture_DXGI
  PRIVATE
    src
    "${PROJECT_TOP}/vendor/directx"
)
