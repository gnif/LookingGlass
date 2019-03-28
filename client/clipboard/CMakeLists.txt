cmake_minimum_required(VERSION 3.0)
project(clipboard LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(CLIPBOARD_PKGCONFIG REQUIRED
	x11
	xfixes
)

add_library(clipboard STATIC
	src/x11.c
)

target_link_libraries(clipboard
	${CLIPBOARD_PKGCONFIG_LIBRARIES}
)

target_include_directories(clipboard
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
	PRIVATE
		src
		${CLIPBOARD_PKGCONFIG_INCLUDE_DIRS}
)
