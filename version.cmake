if (EXISTS ${PROJECT_TOP}/VERSION)
	file(READ ${PROJECT_TOP}/VERSION GIT_REV)
else()
	execute_process(
		COMMAND git describe --always --abbrev=10 --dirty=+ --tags
		WORKING_DIRECTORY "${PROJECT_TOP}"
		OUTPUT_VARIABLE GIT_REV
		ERROR_QUIET)
endif()


if ("${GIT_REV}" STREQUAL "")
	set(GIT_REV "UNKNOWN")
endif()

string(STRIP "${GIT_REV}" GIT_VERSION)
set(BUILD_VERSION "const char * BUILD_VERSION = \"${GIT_VERSION}\";")

string(TIMESTAMP YEAR "%Y")
set(HEADER "\
#define LG_VERSION_STR \"${GIT_VERSION}\"\n\
#define LG_CURRENT_YEAR ${YEAR}\n")

if(EXISTS "${CMAKE_BINARY_DIR}/version.c")
	file(READ ${CMAKE_BINARY_DIR}/version.c BUILD_VERSION_)
else()
	set(BUILD_VERSION_ "")
endif()

if (NOT "${BUILD_VERSION}" STREQUAL "${BUILD_VERSION_}")
	file(WRITE ${CMAKE_BINARY_DIR}/version.c "${BUILD_VERSION}")
	file(WRITE ${CMAKE_BINARY_DIR}/VERSION   "${GIT_VERSION}")
	file(WRITE ${CMAKE_BINARY_DIR}/include/version.h "${HEADER}")
endif()
