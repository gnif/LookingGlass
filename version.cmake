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

	string(STRIP "${GIT_REV}" GIT_REV)
endif()

set(GIT_VERSION "const char * BUILD_VERSION = \"${GIT_REV}\";")

if(EXISTS ${CMAKE_BINARY_DIR}/version.c)
	file(READ ${CMAKE_BINARY_DIR}/version.c GIT_VERSION_)
else()
	if (EXISTS ${PROJECT_TOP}/VERSION)
		file(READ ${PROJECT_TOP}/VERSION GIT_VERSION_)
	else()
		set(GIT_VERSION_ "UNKNOWN")
	endif()
endif()

if (NOT "${GIT_VERSION}" STREQUAL "${GIT_VERSION_}")
	file(WRITE ${CMAKE_BINARY_DIR}/version.c "${GIT_VERSION}")
	file(WRITE ${CMAKE_BINARY_DIR}/VERSION   "${GIT_REV}")
endif()
