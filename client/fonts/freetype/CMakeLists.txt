cmake_minimum_required(VERSION 3.0)
project(font_freetype LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(FONT_FREETYPE_PKGCONFIG REQUIRED
	freetype2
	fontconfig
)

add_library(font_freetype STATIC
	src/freetype.c
)

target_link_libraries(font_freetype
	${FONT_FREETYPE_PKGCONFIG_LIBRARIES}
	lg_common
)

target_include_directories(font_freetype
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
	PRIVATE
		src
		${FONT_FREETYPE_PKGCONFIG_INCLUDE_DIRS}
)
