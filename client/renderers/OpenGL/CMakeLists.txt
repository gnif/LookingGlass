cmake_minimum_required(VERSION 3.0)
project(renderer_Opengl LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(RENDERER_OPENGL_PKGCONFIG REQUIRED
	gl
)

add_library(renderer_OpenGL STATIC
	opengl.c
)

target_link_libraries(renderer_OpenGL
	${RENDERER_OPENGL_PKGCONFIG_LIBRARIES}
	lg_common
	fonts
)

target_include_directories(renderer_OpenGL
	PRIVATE
		src
		${RENDERER_OPENGL_PKGCONFIG_INCLUDE_DIRS}
)
