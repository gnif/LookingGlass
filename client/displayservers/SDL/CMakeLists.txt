cmake_minimum_required(VERSION 3.0)
project(displayserver_SDL LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(DISPLAYSERVER_SDL_PKGCONFIG REQUIRED
	sdl2
)

add_library(displayserver_SDL STATIC
	sdl.c
)

target_link_libraries(displayserver_SDL
	${DISPLAYSERVER_SDL_PKGCONFIG_LIBRARIES}
	${DISPLAYSERVER_SDL_OPT_PKGCONFIG_LIBRARIES}
	lg_common
)

target_include_directories(displayserver_SDL
	PRIVATE
		src
		${DISPLAYSERVER_SDL_PKGCONFIG_INCLUDE_DIRS}
		${DISPLAYSERVER_SDL_OPT_PKGCONFIG_INCLUDE_DIRS}
)
