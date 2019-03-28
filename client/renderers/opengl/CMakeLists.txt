cmake_minimum_required(VERSION 3.0)
project(renderer_opengl LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(RENDERER_OPENGL_PKGCONFIG REQUIRED
	gl
	glu
)

add_library(renderer_opengl STATIC
	opengl.c
)

target_link_libraries(renderer_opengl
	${RENDERER_OPENGL_PKGCONFIG_LIBRARIES}
	decoders
)

target_include_directories(renderer_opengl
	PRIVATE
		src
		${RENDERER_OPENGL_PKGCONFIG_INCLUDE_DIRS}
)
