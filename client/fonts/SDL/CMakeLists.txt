cmake_minimum_required(VERSION 3.0)
project(font_SDL LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(FONT_SDL_PKGCONFIG REQUIRED
	SDL2_ttf
	fontconfig
)

add_library(font_SDL STATIC
	src/sdl.c
)

target_link_libraries(font_SDL
	${FONT_SDL_PKGCONFIG_LIBRARIES}
	lg_common
)

target_include_directories(font_SDL
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
	PRIVATE
		src
		${FONT_SDL_PKGCONFIG_INCLUDE_DIRS}
)
