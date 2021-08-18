cmake_minimum_required(VERSION 3.0)
project(looking-glass-obs C)

get_filename_component(PROJECT_TOP "${PROJECT_SOURCE_DIR}/.." ABSOLUTE)
list(APPEND CMAKE_MODULE_PATH "${PROJECT_TOP}/cmake/" "${PROJECT_SOURCE_DIR}/cmake/")

include(CheckSubmodule)
include(GNUInstallDirs)
include(CheckCCompilerFlag)
include(FeatureSummary)

set(OPTIMIZE_FOR_NATIVE_DEFAULT ON)
include(OptimizeForNative) # option(OPTIMIZE_FOR_NATIVE)
include(UninstallTarget)

option(ENABLE_BACKTRACE "Enable backtrace support on crash" ON)
add_feature_info(ENABLE_BACKTRACE ENABLE_BACKTRACE "Backtrace support.")

add_compile_options(
  "-Wall"
  "-Werror"
  "-Wfatal-errors"
  "-ffast-math"
  "-fdata-sections"
  "-ffunction-sections"
  "-fpic"
  "$<$<CONFIG:DEBUG>:-O0;-g3;-ggdb>"
)

set(EXE_FLAGS "-Wl,--gc-sections")
set(CMAKE_C_STANDARD 11)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(OBS_PLUGIN_DIR 64bit)
else()
	set(OBS_PLUGIN_DIR 32bit)
endif()

if(USER_INSTALL)
	if(DEFINED ENV{XDG_CONFIG_HOME})
		set(OBS_PLUGIN_PREFIX $ENV{XDG_CONFIG_HOME})
	else()
		set(OBS_PLUGIN_PREFIX $ENV{HOME}/.config)
	endif()
	set(OBS_PLUGIN_PREFIX ${OBS_PLUGIN_PREFIX}/obs-studio/plugins)
else()
	set(OBS_PLUGIN_PREFIX ${CMAKE_INSTALL_DATADIR}/obs/obs-plugins)
endif()

add_definitions(-D ATOMIC_LOCKING)

add_custom_command(
	OUTPUT	${CMAKE_BINARY_DIR}/version.c
		${CMAKE_BINARY_DIR}/_version.c
	COMMAND ${CMAKE_COMMAND} -D PROJECT_TOP=${PROJECT_TOP} -P
		${PROJECT_TOP}/version.cmake
)

include_directories(
	${PROJECT_SOURCE_DIR}/include
	${CMAKE_BINARY_DIR}/include
)

link_libraries(
	${CMAKE_DL_LIBS}
	rt
	m
)

set(SOURCES
	${CMAKE_BINARY_DIR}/version.c
	main.c
	lg.c
)

add_subdirectory("${PROJECT_TOP}/common"          "${CMAKE_BINARY_DIR}/common")
add_subdirectory("${PROJECT_TOP}/repos/LGMP/lgmp" "${CMAKE_BINARY_DIR}/lgmp"  )

add_library(looking-glass-obs SHARED ${SOURCES})
target_link_libraries(looking-glass-obs
	${EXE_FLAGS}
	lg_common
	lgmp
)

install(TARGETS looking-glass-obs
	LIBRARY DESTINATION ${OBS_PLUGIN_PREFIX}/${CMAKE_PROJECT_NAME}/bin/${OBS_PLUGIN_DIR}
)

feature_summary(WHAT ENABLED_FEATURES DISABLED_FEATURES)
