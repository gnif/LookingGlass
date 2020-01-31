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

add_compile_options(
  "-Wall"
  "-Werror"
  "-Wfatal-errors"
  "-ffast-math"
  "-fdata-sections"
  "-ffunction-sections"
  "$<$<CONFIG:DEBUG>:-O0;-g3;-ggdb>"
)

set(EXE_FLAGS "-Wl,--gc-sections")
set(CMAKE_C_STANDARD 11)

find_package(PkgConfig)
pkg_check_modules(PKGCONFIG REQUIRED
	sdl2
	x11
)

pkg_check_modules(PKGCONFIG_OPT
	xi
)

execute_process(
	COMMAND			cat ../VERSION
	WORKING_DIRECTORY	${PROJECT_SOURCE_DIR}
	OUTPUT_VARIABLE		BUILD_VERSION
	OUTPUT_STRIP_TRAILING_WHITESPACE
)

find_package(GMP)

add_definitions(-D BUILD_VERSION='"${BUILD_VERSION}"')
add_definitions(-D ATOMIC_LOCKING)
add_definitions(-D GL_GLEXT_PROTOTYPES)
get_filename_component(PROJECT_TOP "${PROJECT_SOURCE_DIR}/.." ABSOLUTE)

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
	src/main.c
	src/app.c
	src/config.c
	src/lg-renderer.c
	src/ll.c
	src/utils.c
)

add_subdirectory("${PROJECT_TOP}/common"          "${CMAKE_BINARY_DIR}/common"   )
add_subdirectory("${PROJECT_TOP}/repos/LGMP/lgmp" "${CMAKE_BINARY_DIR}/LGMP"     )
add_subdirectory("${PROJECT_TOP}/repos/PureSpice" "${CMAKE_BINARY_DIR}/PureSpice")

add_subdirectory(renderers)
add_subdirectory(clipboards)
add_subdirectory(fonts)
add_subdirectory(decoders)

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

feature_summary(WHAT ENABLED_FEATURES DISABLED_FEATURES)
