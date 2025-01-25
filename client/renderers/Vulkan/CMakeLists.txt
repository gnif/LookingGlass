cmake_minimum_required(VERSION 3.5)
project(renderer_Vulkan LANGUAGES C CXX)

add_library(renderer_Vulkan STATIC
  vulkan.c
)

target_link_libraries(renderer_Vulkan
  lg_common
)

target_include_directories(renderer_Vulkan
  PRIVATE
    src
)
