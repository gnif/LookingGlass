cmake_minimum_required(VERSION 3.0)
project(clipboard_Wayland LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(CLIPBOARD_PKGCONFIG REQUIRED
	wayland-client
)

add_library(clipboard_Wayland STATIC
	src/wayland.c
)

target_link_libraries(clipboard_Wayland
	${CLIPBOARD_PKGCONFIG_LIBRARIES}
	lg_common
)

target_include_directories(clipboard_Wayland
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
	PRIVATE
		src
		${CLIPBOARD_PKGCONFIG_INCLUDE_DIRS}
)
