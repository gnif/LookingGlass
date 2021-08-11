cmake_minimum_required(VERSION 3.0)
project(capture_DXGI LANGUAGES C)

add_library(capture_DXGI STATIC
	src/dxgi.c
)

add_definitions("-DCOBJMACROS -DINITGUID")

target_link_libraries(capture_DXGI
	lg_common
	d3d11
	dxgi
)

target_include_directories(capture_DXGI
	PRIVATE
		src
)
