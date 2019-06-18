cmake_minimum_required(VERSION 3.0)
project(lg_common LANGUAGES C)

include_directories(
	${PROJECT_SOURCE_DIR}/include
)

if(ENABLE_BACKTRACE)
  add_definitions(-DENABLE_BACKTRACE)
endif()

set(COMMON_SOURCES
  src/stringutils.c
  src/stringlist.c
  src/option.c
)

set(LINUX_SOURCES
  src/crash.linux.c
  src/sysinfo.linux.c
)

set(WINDOWS_SOURCES
  src/crash.windows.c
  src/sysinfo.windows.c
)

if(WIN32)
  set(SOURCES ${COMMON_SOURCES} ${WINDOWS_SOURCES})
  add_library(lg_common STATIC ${SOURCES})
else()
  set(SOURCES ${COMMON_SOURCES} ${LINUX_SOURCES})
  add_library(lg_common STATIC ${SOURCES})
  if(ENABLE_BACKTRACE)
    target_link_libraries(lg_common bfd)
  endif()
endif()

target_include_directories(lg_common
	INTERFACE
		include
	PRIVATE
		src
)
