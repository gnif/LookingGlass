cmake_minimum_required(VERSION 3.10)
project(client_platform LANGUAGES C)

add_subdirectory(${LG_CLIENT_PLATFORM})

add_library(client_platform INTERFACE)
target_link_libraries(client_platform INTERFACE
  client_platform_${LG_CLIENT_PLATFORM}
)
