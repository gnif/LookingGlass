cmake_minimum_required(VERSION 3.0)
project(lg_common_platform_code LANGUAGES C)

include_directories(
        ${PROJECT_TOP}/vendor/ivshmem
)

# allow use of functions for Windows 7 or later
add_compile_definitions(WINVER=0x0601 _WIN32_WINNT=0x0601)

if (MINGW)
	# Build our own ntdll.dll import library
	# This tricks MinGW into not linking stuff like memcpy from ntdll.dll instead of mscvrt.dll
	if(NOT CMAKE_DLLTOOL)
		# cmake older than 3.16 doesn't know how to find dlltool
		find_program(CMAKE_DLLTOOL NAMES "x86_64-w64-mingw32-dlltool" "dlltool.exe" DOC "dlltool executable")
	endif()
	add_custom_command(OUTPUT "${PROJECT_BINARY_DIR}/ntdll.a"
		COMMAND "${CMAKE_DLLTOOL}" -d "${PROJECT_SOURCE_DIR}/ntdll.def" -l "${PROJECT_BINARY_DIR}/ntdll.a"
		MAIN_DEPENDENCY "${PROJECT_SOURCE_DIR}/ntdll.def"
		COMMENT "Building import library ntdll.a"
		VERBATIM
	)
	add_custom_target(ntdll_target DEPENDS "${PROJECT_BINARY_DIR}/ntdll.a")
	add_library(ntdll STATIC IMPORTED GLOBAL)
	add_dependencies(ntdll ntdll_target)
	set_target_properties(ntdll PROPERTIES IMPORTED_LOCATION "${PROJECT_BINARY_DIR}/ntdll.a")
endif()

add_library(lg_common_platform_code STATIC
    debug.c
    crash.c
    sysinfo.c
    thread.c
    event.c
    windebug.c
    ivshmem.c
    time.c
    cpuinfo.c
)

target_link_libraries(lg_common_platform_code
	lg_common
	setupapi
	ntdll
)

if (ENABLE_BACKTRACE)
    target_link_libraries(lg_common_platform_code dbghelp)
endif()
