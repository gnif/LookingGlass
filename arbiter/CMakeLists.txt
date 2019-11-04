cmake_minimum_required(VERSION 3.0)
project(looking-glass-arbiter C)

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

execute_process(
	COMMAND			cat ../VERSION
	WORKING_DIRECTORY	${PROJECT_SOURCE_DIR}
	OUTPUT_VARIABLE		BUILD_VERSION
	OUTPUT_STRIP_TRAILING_WHITESPACE
)

add_definitions(-D BUILD_VERSION='"${BUILD_VERSION}"')
get_filename_component(PROJECT_TOP "${PROJECT_SOURCE_DIR}/.." ABSOLUTE)

include_directories(
	${PROJECT_SOURCE_DIR}/include
	${CMAKE_BINARY_DIR}/include
)

set(SOURCES
	src/main.c
)

add_subdirectory("${PROJECT_TOP}/common" "${CMAKE_BINARY_DIR}/common")
add_subdirectory("${PROJECT_TOP}/porthole" "${CMAKE_BINARY_DIR}/porthole")

add_executable(looking-glass-arbiter ${SOURCES})
target_link_libraries(looking-glass-arbiter
	${EXE_FLAGS}
	lg_common
	porthole
)

install(PROGRAMS ${CMAKE_BINARY_DIR}/looking-glass-arbiter DESTINATION bin/ COMPONENT binary)

feature_summary(WHAT ENABLED_FEATURES DISABLED_FEATURES)
