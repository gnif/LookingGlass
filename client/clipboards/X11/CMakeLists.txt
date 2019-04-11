cmake_minimum_required(VERSION 3.0)
project(clipboard_X11 LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(CLIPBOARD_PKGCONFIG REQUIRED
	x11
	xfixes
)

add_library(clipboard_X11 STATIC
	src/x11.c
)

target_link_libraries(clipboard_X11
	${CLIPBOARD_PKGCONFIG_LIBRARIES}
	lg_common
)

target_include_directories(clipboard_X11
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
	PRIVATE
		src
		${CLIPBOARD_PKGCONFIG_INCLUDE_DIRS}
)
