cmake_minimum_required(VERSION 3.0)
project(renderer_EGL LANGUAGES C CXX)

find_package(PkgConfig)
pkg_check_modules(RENDERER_EGL REQUIRED IMPORTED_TARGET
	egl
	gl
)

pkg_check_modules(RENDERER_EGL_OPT IMPORTED_TARGET
	wayland-egl
)

include(MakeObject)
make_object(
	EGL_SHADER
	shader/desktop.vert
	shader/desktop_rgb.frag
	shader/cursor.vert
	shader/cursor_rgb.frag
	shader/cursor_mono.frag
	shader/damage.vert
	shader/damage.frag
	shader/splash_bg.vert
	shader/splash_bg.frag
	shader/splash_logo.vert
	shader/splash_logo.frag
)

make_defines(
	"${CMAKE_CURRENT_SOURCE_DIR}/shader/desktop_rgb.frag"
	"${CMAKE_CURRENT_BINARY_DIR}/shader/desktop_rgb.def.h"
)

add_library(renderer_EGL STATIC
	egl.c
	egldebug.c
	shader.c
	texture.c
	model.c
	desktop.c
	cursor.c
	draw.c
	splash.c
	damage.c
	${EGL_SHADER_OBJS}
	"${EGL_SHADER_INCS}/desktop_rgb.def.h"
	${PROJECT_TOP}/repos/cimgui/imgui/backends/imgui_impl_opengl3.cpp
)

target_compile_definitions(renderer_EGL PRIVATE CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1 IMGUI_IMPL_OPENGL_ES3)

target_link_libraries(renderer_EGL
	PkgConfig::RENDERER_EGL
	lg_common

	cimgui
)
if(RENDERER_EGL_OPT_FOUND)
	target_link_libraries(renderer_EGL
		PkgConfig::RENDERER_EGL_OPT
	)
endif()

target_include_directories(renderer_EGL
	PRIVATE
		src
		${EGL_SHADER_INCS}
)
