cmake_minimum_required(VERSION 3.0)
project(platform_Windows LANGUAGES C)

include_directories(
	${PROJECT_SOURCE_DIR}/include
)

add_library(platform_Windows STATIC
	src/platform.c
	src/windebug.c
	src/mousehook.c
)

add_subdirectory("capture")

FIND_PROGRAM(WINDRES_EXECUTABLE NAMES "windres.exe" DOC "windres executable")
ADD_CUSTOM_COMMAND(TARGET platform_Windows POST_BUILD
	WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
	COMMAND ${WINDRES_EXECUTABLE} -i resource.rc -o "${PROJECT_BINARY_DIR}/resource.o"
	VERBATIM
)

target_link_libraries(platform_Windows
	"${PROJECT_BINARY_DIR}/resource.o"
	capture
	setupapi
)

target_include_directories(platform_Windows
	PRIVATE
		src
)
