cmake_minimum_required(VERSION 3.0)
project(renderers LANGUAGES C)

add_subdirectory(opengl)
add_subdirectory(egl)

add_library(renderers INTERFACE)
target_link_libraries(renderers INTERFACE
	renderer_opengl
	renderer_egl
)
