cmake_minimum_required(VERSION 3.0)
project(platform_Windows LANGUAGES C)

include_directories(
	${PROJECT_SOURCE_DIR}/include
)

add_library(platform_Windows STATIC
	src/platform.c
	src/windebug.c	
)

add_subdirectory("capture")

target_link_libraries(platform_Windows
	capture
	setupapi
)

target_include_directories(platform_Windows
	PRIVATE
		src
)
