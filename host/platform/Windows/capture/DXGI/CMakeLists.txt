cmake_minimum_required(VERSION 3.5)
project(capture_DXGI LANGUAGES C)

add_library(capture_DXGI STATIC
  src/dxgi.c
  src/d3d11.c
  src/ods_capture.c
  src/util.c

  src/pp/downsample.c
  src/pp/hdr16to10.c
  src/pp/rgb24.c
)

target_link_libraries(capture_DXGI
  lg_common
  platform_Windows
  d3d11
  dxgi
  dwmapi
  d3dcompiler
)

target_include_directories(capture_DXGI
  PRIVATE
    src
    "${PROJECT_TOP}/vendor/directx"
)
