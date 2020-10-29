cmake_minimum_required(VERSION 3.0)
project(lg_common_platform_code LANGUAGES C)

include_directories(
	${PROJECT_SOURCE_DIR}/include
	${PROJECT_TOP}
)

add_library(lg_common_platform_code STATIC
    crash.c
    sysinfo.c
    thread.c
    event.c
    ivshmem.c
    time.c
)

if(ENABLE_BACKTRACE)
  target_link_libraries(lg_common_platform_code bfd)
endif()

target_link_libraries(lg_common_platform_code
	lg_common
	pthread
	rt
)
