cmake_minimum_required(VERSION 3.0)
project(looking-glass-host C)

set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake/")

include(CheckCCompilerFlag)
include(FeatureSummary)

option(OPTIMIZE_FOR_NATIVE "Build with -march=native" OFF)
if(OPTIMIZE_FOR_NATIVE)
  CHECK_C_COMPILER_FLAG("-march=native" COMPILER_SUPPORTS_MARCH_NATIVE)
  if(COMPILER_SUPPORTS_MARCH_NATIVE)
    add_compile_options("-march=native")
  endif()
else()
  CHECK_C_COMPILER_FLAG("-march=nehalem" COMPILER_SUPPORTS_MARCH_NEHALEM)
  if(COMPILER_SUPPORTS_MARCH_NEHALEM)
    add_compile_options("-march=nehalem")
  endif()
endif()

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
	${PKGCONFIG_INCLUDE_DIRS}
	${GMP_INCLUDE_DIR}
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
  add_executable(looking-glass-host WIN32 ${SOURCES})
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

install(PROGRAMS ${CMAKE_BINARY_DIR}/looking-glass-host DESTINATION bin/ COMPONENT binary)

feature_summary(WHAT ENABLED_FEATURES DISABLED_FEATURES)
