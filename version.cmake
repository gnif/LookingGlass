execute_process(
	COMMAND git describe --always --long --abbrev=10 --tags
	WORKING_DIRECTORY "${PROJECT_TOP}"
        OUTPUT_VARIABLE GIT_REV
        ERROR_QUIET)

if (NOT "${GIT_REV}" STREQUAL "")
	execute_process(
		COMMAND bash -c "git diff --quiet --exit-code || echo +"
		WORKING_DIRECTORY "${PROJECT_TOP}"
		OUTPUT_VARIABLE GIT_DIFF)
else()
	if (EXISTS ${PROJECT_TOP}/VERSION)
		file(READ ${PROJECT_TOP}/VERSION GIT_REV)
	else()
		set(GIT_REV "UNKNOWN")
		set(GIT_DIFF "")
	endif()
endif()

string(STRIP "${GIT_REV}" GIT_REV)
string(STRIP "${GIT_DIFF}" GIT_DIFF)
set(GIT_VERSION "${GIT_REV}${GIT_DIFF}")
set(BUILD_VERSION "const char * BUILD_VERSION = \"${GIT_VERSION}\";")

if(EXISTS "${CMAKE_BINARY_DIR}/version.c")
	file(READ ${CMAKE_BINARY_DIR}/version.c BUILD_VERSION_)
else()
	set(BUILD_VERSION_ "")
endif()

if (NOT "${BUILD_VERSION}" STREQUAL "${BUILD_VERSION_}")
	file(WRITE ${CMAKE_BINARY_DIR}/version.c "${BUILD_VERSION}")
	file(WRITE ${CMAKE_BINARY_DIR}/VERSION   "${GIT_VERSION}")
endif()
