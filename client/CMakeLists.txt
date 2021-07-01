cmake_minimum_required(VERSION 3.0)
project(looking-glass-client C)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake/")

include(GNUInstallDirs)
include(CheckCCompilerFlag)
include(FeatureSummary)

option(OPTIMIZE_FOR_NATIVE "Build with -march=native" ON)
if(OPTIMIZE_FOR_NATIVE)
  CHECK_C_COMPILER_FLAG("-march=native" COMPILER_SUPPORTS_MARCH_NATIVE)
  if(COMPILER_SUPPORTS_MARCH_NATIVE)
    add_compile_options("-march=native")
  endif()
else()
  add_compile_options("-march=nehalem" "-mtune=generic")
endif()

option(ENABLE_OPENGL "Enable the OpenGL renderer"       ON)
add_feature_info(ENABLE_OPENGL ENABLE_OPENGL "Legacy OpenGL renderer.")

option(ENABLE_EGL    "Enable the EGL renderer"          ON)
add_feature_info(ENABLE_EGL ENABLE_EGL "EGL renderer.")

option(ENABLE_BACKTRACE "Enable backtrace support on crash" ON)
add_feature_info(ENABLE_BACKTRACE ENABLE_BACKTRACE "Backtrace support.")

option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
add_feature_info(ENABLE_ASAN ENABLE_ASAN "AddressSanitizer support.")

option(ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
add_feature_info(ENABLE_UBSAN ENABLE_UBSAN "UndefinedBehaviorSanitizer support.")

option(ENABLE_SDL "Build with SDL support" OFF)
add_feature_info(ENABLE_SDL ENABLE_SDL "SDL support.")

option(ENABLE_X11 "Build with X11 support" ON)
add_feature_info(ENABLE_X11 ENABLE_X11 "X11 support.")

option(ENABLE_WAYLAND "Build with Wayland support" ON)
add_feature_info(ENABLE_WAYLAND ENABLE_WAYLAND "Wayland support.")

option(ENABLE_LIBDECOR "Build with libdecor support" OFF)
add_feature_info(ENABLE_LIBDECOR ENABLE_LIBDECOR "libdecor support.")

if (NOT ENABLE_SDL AND NOT ENABLE_X11 AND NOT ENABLE_WAYLAND)
  message(FATAL_ERROR "One of ENABLE_SDL, ENABLE_X11, or ENABLE_WAYLAND must be on")
endif()

add_compile_options(
  "-Wall"
  "-Wextra"
  "-Wno-sign-compare"
  "-Wno-unused-parameter"
  "$<$<C_COMPILER_ID:GNU>:-Wimplicit-fallthrough=2>"
  "-Werror"
  "-Wfatal-errors"
  "-ffast-math"
  "-fdata-sections"
  "-ffunction-sections"
  "$<$<CONFIG:DEBUG>:-O0;-g3;-ggdb>"
)

set(EXE_FLAGS "-Wl,--gc-sections -z noexecstack")
set(CMAKE_C_STANDARD 11)

if (ENABLE_OPENGL)
  add_definitions(-D ENABLE_OPENGL)
endif()

if (ENABLE_EGL)
  add_definitions(-D ENABLE_EGL)
endif()

if(ENABLE_ASAN)
  add_compile_options("-fno-omit-frame-pointer" "-fsanitize=address")
  set(EXE_FLAGS "${EXE_FLAGS} -fno-omit-frame-pointer -fsanitize=address")
endif()

if(ENABLE_UBSAN)
  add_compile_options("-fsanitize=undefined")
  set(EXE_FLAGS "${EXE_FLAGS} -fsanitize=undefined")
endif()

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
	${GMP_INCLUDE_DIR}
)

link_libraries(
	${GMP_LIBRARIES}
	${CMAKE_DL_LIBS}
	rt
	m
)

set(SOURCES
	${CMAKE_BINARY_DIR}/version.c
	src/main.c
	src/core.c
	src/app.c
	src/config.c
	src/keybind.c
	src/lg-renderer.c
	src/ll.c
	src/util.c
	src/clipboard.c
	src/kb.c
	src/egl_dynprocs.c
)

add_subdirectory("${PROJECT_TOP}/common"          "${CMAKE_BINARY_DIR}/common"   )
add_subdirectory("${PROJECT_TOP}/repos/LGMP/lgmp" "${CMAKE_BINARY_DIR}/LGMP"     )
add_subdirectory("${PROJECT_TOP}/repos/PureSpice" "${CMAKE_BINARY_DIR}/PureSpice")

add_subdirectory(displayservers)
add_subdirectory(renderers)
add_subdirectory(fonts)

add_executable(looking-glass-client ${SOURCES})
target_link_libraries(looking-glass-client
	${EXE_FLAGS}
	lg_common
	displayservers
	lgmp
	purespice
	renderers
	fonts
)

install(TARGETS looking-glass-client
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	COMPONENT binary)

feature_summary(WHAT ENABLED_FEATURES DISABLED_FEATURES)
