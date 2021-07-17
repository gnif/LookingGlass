cmake_minimum_required(VERSION 3.0)
project(platform_Windows LANGUAGES C)

include_directories(
	${PROJECT_SOURCE_DIR}/include
)

add_library(platform_Windows STATIC
	src/platform.c
	src/service.c
	src/mousehook.c
	src/force_compose.c
	src/delay.c
)

# allow use of functions for Windows Vista or later
add_definitions(-D _WIN32_WINNT=0x6000)

add_subdirectory("capture")

FIND_PROGRAM(WINDRES_EXECUTABLE NAMES "x86_64-w64-mingw32-windres" "windres.exe" DOC "windres executable")
ADD_CUSTOM_COMMAND(TARGET platform_Windows POST_BUILD
	WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
	COMMAND ${WINDRES_EXECUTABLE} -i resource.rc -o "${PROJECT_BINARY_DIR}/resource.o"
	VERBATIM
)

target_link_libraries(platform_Windows
	"${PROJECT_BINARY_DIR}/resource.o"
	lg_common
	capture

	userenv
	wtsapi32
	psapi
	shlwapi
	powrprof
)

target_include_directories(platform_Windows
	PRIVATE
		src
)

# these are for the nsis installer generator
configure_file("${PROJECT_SOURCE_DIR}/installer.nsi" "${PROJECT_BINARY_DIR}/installer.nsi" COPYONLY)
configure_file("${PROJECT_TOP}/resources/icon.ico"   "${PROJECT_BINARY_DIR}/icon.ico"      COPYONLY)
configure_file("${PROJECT_TOP}/LICENSE"              "${PROJECT_BINARY_DIR}/LICENSE.txt"   COPYONLY)
