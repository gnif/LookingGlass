cmake_minimum_required(VERSION 3.0)
project(lg_common_platform_code LANGUAGES C)

include_directories(
        ${PROJECT_TOP}/vendor/ivshmem
)

# allow use of functions for Windows 7 or later
add_compile_definitions(WINVER=0x0601 _WIN32_WINNT=0x0601)

add_library(lg_common_platform_code STATIC
    debug.c
    crash.c
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
