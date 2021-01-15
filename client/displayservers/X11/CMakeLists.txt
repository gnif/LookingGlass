cmake_minimum_required(VERSION 3.0)
project(displayserver_X11 LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(DISPLAYSERVER_X11_PKGCONFIG REQUIRED
	x11
	xfixes
)

pkg_check_modules(DISPLAYSERVER_X11_OPT_PKGCONFIG
	xi
)

add_library(displayserver_X11 STATIC
	x11.c
)

target_link_libraries(displayserver_X11
	${DISPLAYSERVER_X11_PKGCONFIG_LIBRARIES}
	${DISPLAYSERVER_X11_OPT_PKGCONFIG_LIBRARIES}
	lg_common
)

target_include_directories(displayserver_X11
	PRIVATE
		src
		${DISPLAYSERVER_X11_PKGCONFIG_INCLUDE_DIRS}
		${DISPLAYSERVER_X11_OPT_PKGCONFIG_INCLUDE_DIRS}
)
