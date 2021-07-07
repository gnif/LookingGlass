cmake_minimum_required(VERSION 3.0)
project(lg_common_platform_code LANGUAGES C)

include_directories(
        ${PROJECT_TOP}/vendor/ivshmem
)

add_library(lg_common_platform_code STATIC
    debug.c
    crash.c
    dpi.c
    sysinfo.c
    thread.c
    event.c
    windebug.c
    ivshmem.c
    time.c
)

target_link_libraries(lg_common_platform_code
	lg_common
	setupapi
)

if (ENABLE_BACKTRACE)
    target_link_libraries(lg_common_platform_code dbghelp)
endif()
