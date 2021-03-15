cmake_minimum_required(VERSION 3.0)
project(capture_XCB LANGUAGES C)

add_library(capture_XCB STATIC
	src/xcb.c
)

target_link_libraries(capture_XCB
	lg_common
	xcb
	xcb-shm
	xcb-xfixes
)

target_include_directories(capture_XCB
	PRIVATE
		src
)
