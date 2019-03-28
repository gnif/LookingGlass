cmake_minimum_required(VERSION 3.0)
project(renderer_egl LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(RENDERER_EGL_PKGCONFIG REQUIRED
	egl
	wayland-egl
	gl
)

include(MakeObject)
make_object(
	EGL_SHADER
	shader/desktop.vert
	shader/desktop_rgb.frag
	shader/desktop_yuv.frag
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
	${EGL_SHADER_OBJS}
)

target_link_libraries(renderer_egl
	${RENDERER_EGL_PKGCONFIG_LIBRARIES}
)

target_include_directories(renderer_egl
	PRIVATE
		src
		${EGL_SHADER_INCS}
		${RENDERER_EGL_PKGCONFIG_INCLUDE_DIRS}
)
