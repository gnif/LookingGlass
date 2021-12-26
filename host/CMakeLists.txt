cmake_minimum_required(VERSION 3.0)
project(looking-glass-host C)

get_filename_component(PROJECT_TOP "${PROJECT_SOURCE_DIR}/.." ABSOLUTE)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
	message(FATAL_ERROR
		"\n"
		"In-source builds are not supported\n"
		"See build instructions provided in: "
		"${PROJECT_TOP}/doc/build.rst\n"
		"Refusing to continue"
	)
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_TOP}/cmake/" "${PROJECT_SOURCE_DIR}/cmake/")

set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O3 -g -DNDEBUG" CACHE STRING "compiler flags" FORCE)
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O3 -g -DNDEBUG" CACHE STRING "compiler flags" FORCE)

include(CheckSubmodule)
include(GNUInstallDirs)
include(CheckCCompilerFlag)
include(FeatureSummary)

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
  "$<$<CONFIG:DEBUG>:-O0;-g3;-ggdb>"
)
set(CMAKE_C_STANDARD  11)

if(WIN32)
  add_compile_options("-fdebug-prefix-map=${PROJECT_TOP}/=")
endif()

add_custom_command(
	OUTPUT	${CMAKE_BINARY_DIR}/version.c
    ${CMAKE_BINARY_DIR}/include/version.h
		${CMAKE_BINARY_DIR}/_version.c
	COMMAND ${CMAKE_COMMAND} -D PROJECT_TOP=${PROJECT_TOP} -P
		${PROJECT_TOP}/version.cmake
)

include_directories(
	${PROJECT_SOURCE_DIR}/include
	${CMAKE_BINARY_DIR}/include
)

#link_libraries(
#)

set(SOURCES
	${CMAKE_BINARY_DIR}/version.c
	src/app.c
)

add_subdirectory("${PROJECT_TOP}/common"          "${CMAKE_BINARY_DIR}/common")
add_subdirectory("${PROJECT_TOP}/repos/LGMP/lgmp" "${CMAKE_BINARY_DIR}/lgmp"  )
add_subdirectory(platform)

if(WIN32)
  set_source_files_properties(platform/Windows/resource.rc PROPERTIES
    OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/platform/Windows/app.manifest;${PROJECT_TOP}/resources/icon.ico;${CMAKE_BINARY_DIR}/include/version.h"
  )
  add_executable(looking-glass-host WIN32
    platform/Windows/resource.rc
    ${SOURCES}
  )
else()
  add_executable(looking-glass-host ${SOURCES})
endif()
target_link_libraries(looking-glass-host
	lg_common
	platform
	lgmp
)

if(WIN32)
  set_target_properties(looking-glass-host PROPERTIES LINK_FLAGS "-Wl,--gc-sections -Wl,--nxcompat")
else()
  set_target_properties(looking-glass-host PROPERTIES LINK_FLAGS "-Wl,--gc-sections -z noexecstack")
endif()

install(TARGETS looking-glass-host
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	COMPONENT binary)

feature_summary(WHAT ENABLED_FEATURES DISABLED_FEATURES)
