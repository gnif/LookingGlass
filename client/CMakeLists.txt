cmake_minimum_required(VERSION 3.0)
project(looking-glass-client C)

set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake/")

include(GNUInstallDirs)
include(CheckCCompilerFlag)

option(OPTIMIZE_FOR_NATIVE "Build with -march=native" ON)
if(OPTIMIZE_FOR_NATIVE)
  CHECK_C_COMPILER_FLAG("-march=native" COMPILER_SUPPORTS_MARCH_NATIVE)
  if(COMPILER_SUPPORTS_MARCH_NATIVE)
    add_compile_options("-march=native")
  endif()
endif()

add_compile_options(
  "-Wall"
  "-Werror"
  "-Wfatal-errors"
  "-ffast-math"
  "-fdata-sections"
  "-ffunction-sections"
  "$<$<CONFIG:DEBUG>:-O0;-g3;-ggdb>"
)
set(CMAKE_EXE_LINKER FLAGS "-Wl,--gc-sections")
set(CMAKE_C_STANDARD       11)

find_package(PkgConfig)
pkg_check_modules(PKGCONFIG REQUIRED
	sdl2
	SDL2_ttf
	fontconfig
	x11
	xfixes
	libconfig
)

execute_process(
	COMMAND			git describe --always --long --dirty --abbrev=10 --tags
	WORKING_DIRECTORY	${PROJECT_SOURCE_DIR}
	OUTPUT_VARIABLE		BUILD_VERSION
	OUTPUT_STRIP_TRAILING_WHITESPACE
)

find_package(GMP)

add_definitions(-D BUILD_VERSION='"${BUILD_VERSION}"')
add_definitions(-D ATOMIC_LOCKING)
add_definitions(-D GL_GLEXT_PROTOTYPES)

include_directories(
	${PROJECT_SOURCE_DIR}/include
	${PROJECT_SOURCE_DIR}/../common
	${PKGCONFIG_INCLUDE_DIRS}
	${GMP_INCLUDE_DIR}
)

link_libraries(
	${PKGCONFIG_LIBRARIES}
	${GMP_LIBRARIES}
	rt m
)

set(SOURCES
	src/main.c
	src/lg-renderer.c
	src/lg-fonts.c
	src/ll.c
	src/utils.c
	clipboard/x11.c
	decoders/null.c
	decoders/yuv420.c
	fonts/sdl.c
)

add_subdirectory(spice)
add_subdirectory(renderers)

add_executable(looking-glass-client ${SOURCES})
target_compile_options(looking-glass-client PUBLIC ${PKGCONFIG_CFLAGS_OTHER})
target_link_libraries(looking-glass-client
	spice
	renderers
)
