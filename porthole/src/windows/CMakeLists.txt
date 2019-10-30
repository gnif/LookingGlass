cmake_minimum_required(VERSION 3.0)
project(porthole-windows LANGUAGES C)

add_library(porthole-windows STATIC
	device.c
)

target_link_libraries(porthole-windows
	setupapi
	lg_common
)

target_include_directories(porthole-windows
	PRIVATE
		src
)
