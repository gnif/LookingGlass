cmake_minimum_required(VERSION 3.0)
project(lg_common LANGUAGES C)

include_directories(
	${PROJECT_SOURCE_DIR}/include
)

add_definitions(-D_GNU_SOURCE)

if(ENABLE_BACKTRACE)
  add_definitions(-DENABLE_BACKTRACE)
endif()

add_subdirectory(src/platform)

set(COMMON_SOURCES
  src/stringutils.c
  src/stringlist.c
  src/option.c
  src/framebuffer.c
  src/KVMFR.c
)

add_library(lg_common STATIC ${COMMON_SOURCES})
target_link_libraries(lg_common lg_common_platform)

target_include_directories(lg_common
	INTERFACE
		include
	PRIVATE
		src
)
