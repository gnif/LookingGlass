cmake_minimum_required(VERSION 3.10)
project(transport_Test LANGUAGES C)

add_library(transport_Test STATIC
  test.c
)

target_compile_options(transport_Test PRIVATE
  -fno-fast-math
)

target_link_libraries(transport_Test
  lg_common
)
