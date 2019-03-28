cmake_minimum_required(VERSION 3.6)
project(renderer_egl LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(RENDERER_EGL_PKGCONFIG REQUIRED
	egl
	wayland-egl
)

add_library(renderer_egl STATIC
	egl.c
	shader.c
	texture.c
	model.c
	desktop.c
	cursor.c
	fps.c
	draw.c
	splash.c
	alert.c
)

target_link_libraries(renderer_egl
	${RENDERER_EGL_PKGCONFIG_LIBRARIES}
)

target_include_directories(renderer_egl
	PRIVATE
		src
		${RENDERER_EGL_PKGCONFIG_INCLUDE_DIRS}
)
