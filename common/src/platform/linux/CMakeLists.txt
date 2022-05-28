cmake_minimum_required(VERSION 3.0)
project(lg_common_platform_code LANGUAGES C)

include_directories(
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_TOP}
)

add_library(lg_common_platform_code STATIC
  debug.c
  crash.c
  sysinfo.c
  thread.c
  event.c
  ivshmem.c
  time.c
  paths.c
  open.c
  cpuinfo.c
)

if(ENABLE_BACKTRACE)
  find_package(BFD)
  target_link_libraries(lg_common_platform_code ${BFD_LIBRARIES})
endif()

target_link_libraries(lg_common_platform_code
  lg_common
  pthread
  rt
)
