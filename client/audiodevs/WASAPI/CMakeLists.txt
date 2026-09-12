cmake_minimum_required(VERSION 3.10)
project(audiodev_WASAPI LANGUAGES C)

add_library(audiodev_WASAPI STATIC
  capture_queue.c
  format.c
  guids.c
  wasapi.c
)

target_link_libraries(audiodev_WASAPI
  avrt
  lg_common
  m
  ole32
  uuid
)

target_include_directories(audiodev_WASAPI
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
