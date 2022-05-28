cmake_minimum_required(VERSION 3.0)
project(renderer_Opengl LANGUAGES C CXX)

find_package(PkgConfig)
pkg_check_modules(RENDERER_OPENGL REQUIRED IMPORTED_TARGET
  gl
)

add_library(renderer_OpenGL STATIC
  opengl.c
  ${PROJECT_TOP}/repos/cimgui/imgui/backends/imgui_impl_opengl2.cpp
)

target_compile_definitions(renderer_OpenGL PRIVATE CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1)

target_link_libraries(renderer_OpenGL
  PkgConfig::RENDERER_OPENGL
  lg_common

  cimgui
)

target_include_directories(renderer_OpenGL
  PRIVATE
    src
)
