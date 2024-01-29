cmake_minimum_required(VERSION 3.5)
project(capture_D12 LANGUAGES C)

add_library(capture_D12 STATIC
  d12.c
  backend/dd.c
)

add_definitions("-DCOBJMACROS -DINITGUID -DWIDL_C_INLINE_WRAPPERS")

target_link_libraries(capture_D12
  lg_common
  d3d11
  dxgi
  dwmapi
  d3dcompiler
)

target_include_directories(capture_D12
  PRIVATE
    .
    "${PROJECT_TOP}/vendor/directx"
)
