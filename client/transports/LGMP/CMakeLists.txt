cmake_minimum_required(VERSION 3.10)
project(transport_LGMP LANGUAGES C)

add_library(transport_LGMP STATIC
  clipboard.c
  input.c
  lgmp.c
)

target_include_directories(transport_LGMP PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

target_link_libraries(transport_LGMP
  lg_common
  lgmp
)
