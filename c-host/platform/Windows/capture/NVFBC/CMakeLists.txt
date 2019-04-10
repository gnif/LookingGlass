cmake_minimum_required(VERSION 3.0)
project(capture_NVFBC LANGUAGES C CXX)

add_library(capture_NVFBC STATIC
	src/nvFBC.c
	src/wrapper.cpp
)

include_directories("${NVFBC_SDK}\\inc")

target_include_directories(capture_NVFBC
	PRIVATE
		src
)

target_link_libraries(capture_NVFBC
	platform_Windows
)
