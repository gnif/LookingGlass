cmake_minimum_required(VERSION 2.8)
project(looking-glass-client C)

set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake/")

SET(CMAKE_C_FLAGS "-std=gnu99 -g -O3 -march=native -Wall -Werror -Wfatal-errors -ffast-math -fdata-sections -ffunction-sections")
SET(CMAKE_EXE_LINKER_FLAGS "-Wl,--gc-sections")

find_package(PkgConfig)
pkg_check_modules(PKGCONFIG REQUIRED
	sdl2
	SDL2_ttf
	gl
	glu
	egl
	spice-protocol
	fontconfig
	x11
	libconfig
	nettle
	hogweed
)

execute_process(
	COMMAND			git describe --always --long --dirty --abbrev=10 --tags
	WORKING_DIRECTORY	${PROJECT_SOURCE_DIR}
	OUTPUT_VARIABLE		BUILD_VERSION
	OUTPUT_STRIP_TRAILING_WHITESPACE
)

find_package(GMP)

add_definitions(-D BUILD_VERSION='"${BUILD_VERSION}"')
add_definitions(-D USE_NETTLE)
add_definitions(-D ATOMIC_LOCKING)
add_definitions(-D GL_GLEXT_PROTOTYPES)

include_directories(
	${PROJECT_SOURCE_DIR}
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
	main.c
	lg-renderer.c
	lg-fonts.c
	ll.c
	utils.c
	spice/rsa.c
	spice/spice.c
	decoders/null.c
	decoders/yuv420.c
	renderers/opengl.c
	renderers/egl.c
	renderers/egl/shader.c
	renderers/egl/texture.c
	renderers/egl/model.c
	renderers/egl/desktop.c
	renderers/egl/cursor.c
	renderers/egl/fps.c
	renderers/egl/draw.c
	renderers/egl/splash.c
	renderers/egl/alert.c
	fonts/sdl.c
)

add_executable(looking-glass-client ${SOURCES})
target_compile_options(looking-glass-client PUBLIC ${PKGCONFIG_CFLAGS_OTHER})
