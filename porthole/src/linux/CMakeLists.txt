cmake_minimum_required(VERSION 3.0)
project(porthole-linux LANGUAGES C)

add_library(porthole-linux STATIC
	device.c
)

#target_link_libraries(porthole-linux)

target_include_directories(porthole-linux
	PRIVATE
		src
)
