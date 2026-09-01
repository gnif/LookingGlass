cmake_minimum_required(VERSION 3.10)
project(lg_common LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake/")

include_directories(
  ${PROJECT_SOURCE_DIR}/include
)

add_definitions(-D_GNU_SOURCE)

if (CMAKE_C_COMPILER_ID STREQUAL "Clang")
  add_compile_options(
    "-Wno-unknown-warning-option"
  )
endif()

function(_lg_common_add_protocol)
  # Function scope preserves the parent BUILD_TESTING setting while disabling
  # LGProtocol's standalone tests in the nested build.
  set(BUILD_TESTING OFF)
  add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../repos/LGProtocol"
    "${CMAKE_CURRENT_BINARY_DIR}/LGProtocol"
    EXCLUDE_FROM_ALL
  )
endfunction()

_lg_common_add_protocol()

add_subdirectory(src/platform)

set(COMMON_SOURCES
  src/appstrings.c
  src/stringutils.c
  src/stringlist.c
  src/option.c
  src/framebuffer.c
  src/KVMFR.c
  src/countedbuffer.c
  src/rects.c
  src/runningavg.c
  src/ringbuffer.c
  src/vector.c
  src/cpuinfo.c
  src/debug.c
  src/ll.c
)

add_library(lg_common STATIC ${COMMON_SOURCES})
target_link_libraries(lg_common
  PUBLIC
    lg_common_platform
    LGProtocol::LGProtocol
)

if(ENABLE_BACKTRACE)
  target_compile_definitions(lg_common PUBLIC -DENABLE_BACKTRACE)
endif()

target_include_directories(lg_common
  INTERFACE
    include
  PRIVATE
    src
)
