cmake_minimum_required(VERSION 3.0)
project(lg_common LANGUAGES C)

include_directories(
	${PROJECT_SOURCE_DIR}/include
)

add_library(lg_common STATIC
	src/debug.c
)

#target_link_libraries(lg_common
#)

target_include_directories(lg_common
	INTERFACE
		include
	PRIVATE
		src
)
