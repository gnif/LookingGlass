cmake_minimum_required(VERSION 3.6)
project(fonts LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(FONTS_PKGCONFIG REQUIRED
	SDL2_ttf
	fontconfig
)

add_library(fonts STATIC
	src/sdl.c
)

target_link_libraries(fonts
	${FONTS_PKGCONFIG_LIBRARIES}
)

target_include_directories(fonts
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
	PRIVATE
		src
		${FONTS_PKGCONFIG_INCLUDE_DIRS}
)
