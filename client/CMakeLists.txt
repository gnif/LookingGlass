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

option(ENABLE_BACKTRACE "Enable backtrace support on crash" ON)
add_feature_info(ENABLE_BACKTRACE ENABLE_BACKTRACE "Backtrace support.")

option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
add_feature_info(ENABLE_ASAN ENABLE_ASAN "AddressSanitizer support.")

option(ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
add_feature_info(ENABLE_UBSAN ENABLE_UBSAN "UndefinedBehaviorSanitizer support.")

option(ENABLE_X11 "Build with X11 support" ON)
add_feature_info(ENABLE_X11 ENABLE_X11 "X11 support.")

option(ENABLE_WAYLAND "Build with Wayland support" ON)
add_feature_info(ENABLE_WAYLAND ENABLE_WAYLAND "Wayland support.")

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

if(ENABLE_ASAN)
  add_compile_options("-fno-omit-frame-pointer" "-fsanitize=address")
  set(EXE_FLAGS "${EXE_FLAGS} -fno-omit-frame-pointer -fsanitize=address")
endif()

if(ENABLE_UBSAN)
  add_compile_options("-fsanitize=undefined")
  set(EXE_FLAGS "${EXE_FLAGS} -fsanitize=undefined")
endif()

find_package(PkgConfig)
pkg_check_modules(PKGCONFIG REQUIRED
	sdl2
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
)

add_subdirectory("${PROJECT_TOP}/common"          "${CMAKE_BINARY_DIR}/common"   )
add_subdirectory("${PROJECT_TOP}/repos/LGMP/lgmp" "${CMAKE_BINARY_DIR}/LGMP"     )
add_subdirectory("${PROJECT_TOP}/repos/PureSpice" "${CMAKE_BINARY_DIR}/PureSpice")

add_subdirectory(displayservers)
add_subdirectory(renderers)
add_subdirectory(fonts)

add_executable(looking-glass-client ${SOURCES})
target_compile_options(looking-glass-client PUBLIC ${PKGCONFIG_CFLAGS_OTHER} ${PKGCONFIG_OPT_CFLAGS_OTHER})
target_link_libraries(looking-glass-client
	${EXE_FLAGS}
	lg_common
	displayservers
	lgmp
	purespice
	renderers
	fonts
)

install(PROGRAMS ${CMAKE_BINARY_DIR}/looking-glass-client DESTINATION bin/ COMPONENT binary)

feature_summary(WHAT ENABLED_FEATURES DISABLED_FEATURES)
