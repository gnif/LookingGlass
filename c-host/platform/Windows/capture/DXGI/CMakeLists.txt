cmake_minimum_required(VERSION 3.0)
project(capture_DXGI LANGUAGES C)

add_library(capture_DXGI STATIC
	src/dxgi.c
)

add_definitions("-DCOBJMACROS -DINITGUID")

FIND_PROGRAM(DLLTOOL_EXECUTABLE NAMES "x86_64-w64-mingw32-dlltool" "dlltool" "dlltool.exe" DOC "dlltool executable")
ADD_CUSTOM_COMMAND(TARGET capture_DXGI POST_BUILD
	WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/dll"
	COMMAND ${DLLTOOL_EXECUTABLE} --def libd3d11.def --output-lib "${PROJECT_BINARY_DIR}/libd3d11.dll"
	VERBATIM
)

target_link_libraries(capture_DXGI
	lg_common
	${PROJECT_BINARY_DIR}/libd3d11.dll
	dxgi
)

target_include_directories(capture_DXGI
	PRIVATE
		src
)
