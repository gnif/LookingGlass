cmake_minimum_required(VERSION 3.12)
project(renderer_Vulkan LANGUAGES C CXX)

find_package(PkgConfig)
pkg_check_modules(RENDERER_Vulkan REQUIRED IMPORTED_TARGET
  vulkan
)

include(MakeObject)
function(build_vulkan_shaders)
  foreach(shader ${ARGN})
    set(out_f "${CMAKE_CURRENT_BINARY_DIR}/${shader}.spv")
    add_custom_command(OUTPUT "${out_f}"
      COMMAND glslangValidator --target-env vulkan1.0 -o "${out_f}"
              "${CMAKE_CURRENT_SOURCE_DIR}/${shader}"
      MAIN_DEPENDENCY "${CMAKE_CURRENT_SOURCE_DIR}/${shader}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/shader"
      COMMENT "Preprocessing shader ${shader}"
      VERBATIM
    )
  endforeach()

  list(TRANSFORM ARGN APPEND ".spv" OUTPUT_VARIABLE ARGN_SPV)

  set(CMAKE_CURRENT_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
  make_object(
    VULKAN_SHADER
    ${ARGN_SPV}
  )

  set(VULKAN_SHADER_OBJS "${VULKAN_SHADER_OBJS}" PARENT_SCOPE)
  set(VULKAN_SHADER_INCS "${VULKAN_SHADER_INCS}" PARENT_SCOPE)
endfunction()

build_vulkan_shaders(
  shader/basic.frag
  shader/basic.vert
)

add_library(renderer_Vulkan STATIC
  vulkan.c
  ${VULKAN_SHADER_OBJS}
)

target_link_libraries(renderer_Vulkan
  PkgConfig::RENDERER_Vulkan
  lg_common
)

target_include_directories(renderer_Vulkan
  PRIVATE
    src
    ${VULKAN_SHADER_INCS}
)
