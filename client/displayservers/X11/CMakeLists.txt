cmake_minimum_required(VERSION 3.0)
project(displayserver_X11 LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(DISPLAYSERVER_X11_PKGCONFIG REQUIRED
	x11
	xi
	xfixes
	xscrnsaver
	xinerama
)

add_library(displayserver_X11 STATIC
	x11.c
	atoms.c
	clipboard.c
)

add_definitions(-D GLX_GLXEXT_PROTOTYPES)

target_link_libraries(displayserver_X11
	${DISPLAYSERVER_X11_PKGCONFIG_LIBRARIES}
	lg_common
)

target_include_directories(displayserver_X11
	PRIVATE
		src
		${DISPLAYSERVER_X11_PKGCONFIG_INCLUDE_DIRS}
)
