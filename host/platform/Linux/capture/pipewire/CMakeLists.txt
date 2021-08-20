cmake_minimum_required(VERSION 3.0)
project(capture_pipewire LANGUAGES C)

add_library(capture_pipewire STATIC
	src/pipewire.c
)

target_link_libraries(capture_pipewire
	lg_common
)

target_include_directories(capture_pipewire
	PRIVATE
		src
)
