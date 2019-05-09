cmake_minimum_required(VERSION 3.0)
project(lg_common LANGUAGES C)

include_directories(
	${PROJECT_SOURCE_DIR}/include
)

set(SOURCES
  src/option.c
)

if(WIN32)
  add_library(lg_common STATIC src/crash.windows.c ${SOURCES})
else()
  add_library(lg_common STATIC src/crash.linux.c ${SOURCES})
  target_link_libraries(lg_common bfd)
endif()

target_include_directories(lg_common
	INTERFACE
		include
	PRIVATE
		src
)
