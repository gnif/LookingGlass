cmake_minimum_required(VERSION 3.0)
project(lg_common_platform_code LANGUAGES C)

include_directories(
	${PROJECT_SOURCE_DIR}/include
)

add_library(lg_common_platform_code STATIC
    crash.c
    sysinfo.c
)
