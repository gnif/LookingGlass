cmake_minimum_required(VERSION 3.0)
project(displayserver_Wayland LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(DISPLAYSERVER_Wayland_PKGCONFIG REQUIRED
	wayland-client
)

#pkg_check_modules(DISPLAYSERVER_Wayland_OPT_PKGCONFIG
#)

add_library(displayserver_Wayland STATIC
	wayland.c
)

target_link_libraries(displayserver_Wayland
	${DISPLAYSERVER_Wayland_PKGCONFIG_LIBRARIES}
	${DISPLAYSERVER_Wayland_OPT_PKGCONFIG_LIBRARIES}
	lg_common
)

target_include_directories(displayserver_Wayland
	PRIVATE
		src
		${DISPLAYSERVER_Wayland_PKGCONFIG_INCLUDE_DIRS}
		${DISPLAYSERVER_Wayland_OPT_PKGCONFIG_INCLUDE_DIRS}
)

find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols>=1.15)
pkg_get_variable(WAYLAND_PROTOCOLS_BASE wayland-protocols pkgdatadir)

macro(wayland_generate protocol_file output_file)
    add_custom_command(OUTPUT "${output_file}.h"
        COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${protocol_file}" "${output_file}.h"
        DEPENDS "${protocol_file}"
        VERBATIM)

    add_custom_command(OUTPUT "${output_file}.c"
        COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${protocol_file}" "${output_file}.c"
        DEPENDS "${protocol_file}"
        VERBATIM)

    target_sources(displayserver_Wayland PRIVATE "${output_file}.h" "${output_file}.c")
endmacro()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/wayland")
include_directories("${CMAKE_BINARY_DIR}/wayland")
wayland_generate(
    "${WAYLAND_PROTOCOLS_BASE}/unstable/relative-pointer/relative-pointer-unstable-v1.xml"
    "${CMAKE_BINARY_DIR}/wayland/wayland-relative-pointer-unstable-v1-client-protocol")
wayland_generate(
    "${WAYLAND_PROTOCOLS_BASE}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml"
    "${CMAKE_BINARY_DIR}/wayland/wayland-pointer-constraints-unstable-v1-client-protocol")
wayland_generate(
    "${WAYLAND_PROTOCOLS_BASE}/unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml"
    "${CMAKE_BINARY_DIR}/wayland/wayland-keyboard-shortcuts-inhibit-unstable-v1-client-protocol")
wayland_generate(
    "${WAYLAND_PROTOCOLS_BASE}/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml"
    "${CMAKE_BINARY_DIR}/wayland/wayland-idle-inhibit-unstable-v1-client-protocol")

