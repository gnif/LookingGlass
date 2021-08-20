cmake_minimum_required(VERSION 3.0)
project(capture_pipewire LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(CAPTURE_PIPEWIRE REQUIRED IMPORTED_TARGET
	gio-2.0
	gio-unix-2.0
	libpipewire-0.3
	libspa-0.2
)

add_library(capture_pipewire STATIC
	src/pipewire.c
	src/portal.c
)

target_link_libraries(capture_pipewire
	PkgConfig::CAPTURE_PIPEWIRE
	lg_common
)

target_include_directories(capture_pipewire
	PRIVATE
		src
)
