cmake_minimum_required(VERSION 3.0)
project(renderer_EGL LANGUAGES C CXX)

find_package(PkgConfig)
pkg_check_modules(RENDERER_EGL REQUIRED IMPORTED_TARGET
  egl
  gl
)

pkg_check_modules(RENDERER_EGL_OPT IMPORTED_TARGET
  wayland-egl
)

find_program(AWK NAMES gawk mawk original-awk awk)

if(AWK MATCHES ".+-NOTFOUND")
  message(FATAL_ERROR "FATAL: some known version of awk couldn't be found (${AWK}).")
else()
  message(STATUS "Using awk: ${AWK}")
endif()

include(MakeObject)
function(build_shaders header_dir)
  file(GLOB headers "${header_dir}/*.h")
  set(EGL_SHADER_PROCESSED)
  foreach(shader ${ARGN})
    set(out_f "${CMAKE_CURRENT_BINARY_DIR}/${shader}")
    add_custom_command(OUTPUT "${out_f}"
      COMMAND "${AWK}" -f "${CMAKE_CURRENT_SOURCE_DIR}/glsl.include.awk"
              "${CMAKE_CURRENT_SOURCE_DIR}/${shader}" > "${out_f}"
      MAIN_DEPENDENCY "${CMAKE_CURRENT_SOURCE_DIR}/${shader}"
      DEPENDS ${headers}
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/shader"
      COMMENT "Preprocessing shader ${shader}"
      VERBATIM
    )
  endforeach()

  set(CMAKE_CURRENT_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
  make_object(
    EGL_SHADER
    ${ARGN}
  )

  set(EGL_SHADER_OBJS "${EGL_SHADER_OBJS}" PARENT_SCOPE)
  set(EGL_SHADER_INCS "${EGL_SHADER_INCS}" PARENT_SCOPE)
endfunction()

build_shaders(
  shader
  shader/desktop.vert
  shader/desktop_rgb.frag
  shader/cursor.vert
  shader/cursor_rgb.frag
  shader/cursor_mono.frag
  shader/damage.vert
  shader/damage.frag
  shader/basic.vert
  shader/ffx_cas.frag
  shader/ffx_fsr1_easu.frag
  shader/ffx_fsr1_rcas.frag
  shader/downscale.frag
  shader/downscale_lanczos2.frag
  shader/downscale_linear.frag
)

make_defines(
  "${CMAKE_CURRENT_SOURCE_DIR}/shader/desktop_rgb.frag"
  "${CMAKE_CURRENT_BINARY_DIR}/shader/desktop_rgb.def.h"
)

add_library(renderer_EGL STATIC
  egl.c
  egldebug.c
  shader.c
  texture_util.c
  texture.c
  texture_buffer.c
  texture_framebuffer.c
  texture_dmabuf.c
  model.c
  desktop.c
  desktop_rects.c
  cursor.c
  damage.c
  framebuffer.c
  postprocess.c
  ffx.c
  filter.c
  filter_ffx_cas.c
  filter_ffx_fsr1.c
  filter_downscale.c
  ${EGL_SHADER_OBJS}
  "${CMAKE_CURRENT_BINARY_DIR}/shader/desktop_rgb.def.h"
  ${PROJECT_TOP}/repos/cimgui/imgui/backends/imgui_impl_opengl3.cpp
)

target_compile_definitions(renderer_EGL PRIVATE CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1 IMGUI_IMPL_OPENGL_ES3)

target_link_libraries(renderer_EGL
  PkgConfig::RENDERER_EGL
  lg_common

  cimgui
)
if(RENDERER_EGL_OPT_FOUND)
  target_link_libraries(renderer_EGL
    PkgConfig::RENDERER_EGL_OPT
  )
endif()

target_include_directories(renderer_EGL
  PRIVATE
    src
    ${EGL_SHADER_INCS}
)
