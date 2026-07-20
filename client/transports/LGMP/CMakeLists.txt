cmake_minimum_required(VERSION 3.10)
project(transport_LGMP LANGUAGES C)

add_library(transport_LGMP STATIC
  lgmp.c
)

target_link_libraries(transport_LGMP
  lg_common
  lgmp
)
