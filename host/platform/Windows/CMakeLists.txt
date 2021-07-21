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

# allow use of functions for Windows 7 or later
add_compile_definitions(WINVER=0x0601 _WIN32_WINNT=0x0601)

add_subdirectory("capture")

if (MINGW)
	# Build our own ntdll.dll import library
	# This tricks MinGW into not linking stuff like memcpy from ntdll.dll instead of mscvrt.dll
	find_program(DLLTOOL_EXECUTABLE NAMES "x86_64-w64-mingw32-dlltool" "dlltool.exe" DOC "dlltool executable")
	add_custom_command(OUTPUT "${PROJECT_BINARY_DIR}/ntdll.a"
		COMMAND "${DLLTOOL_EXECUTABLE}" -d "${PROJECT_SOURCE_DIR}/ntdll.def" -l "${PROJECT_BINARY_DIR}/ntdll.a"
		MAIN_DEPENDENCY "${PROJECT_SOURCE_DIR}/ntdll.def"
		COMMENT "Building import library ntdll.a"
		VERBATIM
	)
	add_custom_target(ntdll_target DEPENDS "${PROJECT_BINARY_DIR}/ntdll.a")
	add_library(ntdll STATIC IMPORTED GLOBAL)
	add_dependencies(ntdll ntdll_target)
	set_target_properties(ntdll PROPERTIES IMPORTED_LOCATION "${PROJECT_BINARY_DIR}/ntdll.a")
endif()

target_link_libraries(platform_Windows
	lg_common
	capture

	userenv
	ntdll
	wtsapi32
	psapi
	shlwapi
	powrprof
	rpcrt4
)

target_include_directories(platform_Windows
	PRIVATE
		src
)

# these are for the nsis installer generator
configure_file("${PROJECT_SOURCE_DIR}/installer.nsi" "${PROJECT_BINARY_DIR}/installer.nsi" COPYONLY)
configure_file("${PROJECT_TOP}/resources/icon.ico"   "${PROJECT_BINARY_DIR}/icon.ico"      COPYONLY)
configure_file("${PROJECT_TOP}/LICENSE"              "${PROJECT_BINARY_DIR}/LICENSE.txt"   COPYONLY)
