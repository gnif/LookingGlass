cmake_minimum_required(VERSION 3.0)
project(looking-glass-client C)

set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake/")

include(GNUInstallDirs)
include(CheckCCompilerFlag)
include(FeatureSummary)

option(OPTIMIZE_FOR_NATIVE "Build with -march=native" ON)
if(OPTIMIZE_FOR_NATIVE)
  CHECK_C_COMPILER_FLAG("-march=native" COMPILER_SUPPORTS_MARCH_NATIVE)
  if(COMPILER_SUPPORTS_MARCH_NATIVE)
    add_compile_options("-march=native")
  endif()
endif()

option(ENABLE_OPENGL "Enable the OpenGL renderer"       ON)
add_feature_info(ENABLE_OPENGL ENABLE_OPENGL "Legacy OpenGL renderer.")

option(ENABLE_EGL    "Enable the EGL renderer"          ON)
add_feature_info(ENABLE_EGL ENABLE_EGL "EGL renderer.")

option(ENABLE_CB_X11 "Enable X11 clipboard integration" ON)
add_feature_info(ENABLE_CB_X11 ENABLE_CB_X11 "X11 Clipboard Integration.")

option(ENABLE_BACKTRACE "Enable backtrace support on crash" ON)
add_feature_info(ENABLE_BACKTRACE ENABLE_BACKTRACE "Backtrace support.")

option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
add_feature_info(ENABLE_ASAN ENABLE_ASAN "AddressSanitizer support.")

option(ENABLE_WAYLAND "Build with Wayland support" ON)
add_feature_info(ENABLE_WAYLAND ENABLE_WAYLAND "Wayland support.")

add_compile_options(
  "-Wall"
  "-Werror"
  "-Wfatal-errors"
  "-ffast-math"
  "-fdata-sections"
  "-ffunction-sections"
  "$<$<CONFIG:DEBUG>:-O0;-g3;-ggdb>"
)

set(EXE_FLAGS "-Wl,--gc-sections -z noexecstack")
set(CMAKE_C_STANDARD 11)

if(ENABLE_ASAN)
  add_compile_options("-fno-omit-frame-pointer" "-fsanitize=address")
  set(EXE_FLAGS "${EXE_FLAGS} -fno-omit-frame-pointer -fsanitize=address")
endif()

find_package(PkgConfig)
pkg_check_modules(PKGCONFIG REQUIRED
	sdl2
	x11
)

pkg_check_modules(PKGCONFIG_OPT
	xi
)

find_package(GMP)

add_definitions(-D ATOMIC_LOCKING)
add_definitions(-D GL_GLEXT_PROTOTYPES)
get_filename_component(PROJECT_TOP "${PROJECT_SOURCE_DIR}/.." ABSOLUTE)

add_custom_command(
	OUTPUT	${CMAKE_BINARY_DIR}/version.c
		${CMAKE_BINARY_DIR}/_version.c
	COMMAND ${CMAKE_COMMAND} -D PROJECT_TOP=${PROJECT_TOP} -P
		${PROJECT_TOP}/version.cmake
)

include_directories(
	${PROJECT_SOURCE_DIR}/include
	${CMAKE_BINARY_DIR}/include
	${PKGCONFIG_INCLUDE_DIRS} ${PKGCONFIG_OPT_INCLUDE_DIRS}
	${GMP_INCLUDE_DIR}
)

link_libraries(
	${PKGCONFIG_LIBRARIES} ${PKGCONFIG_OPT_LIBRARIES}
	${GMP_LIBRARIES}
	${CMAKE_DL_LIBS}
	rt
	m
)

set(SOURCES
	${CMAKE_BINARY_DIR}/version.c
	src/main.c
	src/app.c
	src/config.c
	src/lg-renderer.c
	src/ll.c
	src/utils.c
	src/wm.c
)

add_subdirectory("${PROJECT_TOP}/common"          "${CMAKE_BINARY_DIR}/common"   )
add_subdirectory("${PROJECT_TOP}/repos/LGMP/lgmp" "${CMAKE_BINARY_DIR}/LGMP"     )
add_subdirectory("${PROJECT_TOP}/repos/PureSpice" "${CMAKE_BINARY_DIR}/PureSpice")

add_subdirectory(renderers)
add_subdirectory(clipboards)
add_subdirectory(fonts)

add_executable(looking-glass-client ${SOURCES})
target_compile_options(looking-glass-client PUBLIC ${PKGCONFIG_CFLAGS_OTHER} ${PKGCONFIG_OPT_CFLAGS_OTHER})
target_link_libraries(looking-glass-client
	${EXE_FLAGS}
	lg_common
	lgmp
	purespice
	renderers
	clipboards
	fonts
)

install(PROGRAMS ${CMAKE_BINARY_DIR}/looking-glass-client DESTINATION bin/ COMPONENT binary)

if(ENABLE_WAYLAND)
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

      target_sources(looking-glass-client PRIVATE "${output_file}.h" "${output_file}.c")
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
endif()

feature_summary(WHAT ENABLED_FEATURES DISABLED_FEATURES)
