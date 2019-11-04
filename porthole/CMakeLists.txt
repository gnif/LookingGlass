cmake_minimum_required(VERSION 3.0)
project(porthole LANGUAGES C)

include_directories(
	${PROJECT_SOURCE_DIR}/include
)

add_library(porthole STATIC
  src/porthole.c
)

if(WIN32)
  add_subdirectory(src/windows)
  set(PLATFORM "windows")
else()
  add_subdirectory(src/linux)
  set(PLATFORM "linux")
endif()

target_link_libraries(porthole
	PRIVATE
		porthole-${PLATFORM}
		lg_common
		pthread
)

target_include_directories(porthole
	INTERFACE
		include
	PRIVATE
		src
)
