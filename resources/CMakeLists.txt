cmake_minimum_required(VERSION 3.10)
project(lg_resources LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake/")

include(MakeObject)

function(build_resources)
	make_object(
		LG_RESOURCES
		${ARGN}
	)
	set(LG_RESOURCES_OBJS "${LG_RESOURCES_OBJS}" PARENT_SCOPE)
	set(LG_RESOURCES_INCS "${LG_RESOURCES_INCS}" PARENT_SCOPE)
endfunction()

set(RESOURCES_SOURCES
	icondata.c
)

build_resources(
	lg-logo.svg
	status/spice.svg
	status/recording.svg
	no-input-cursor/16.xcur
	no-input-cursor/32.xcur
)

add_library(lg_resources STATIC ${RESOURCES_SOURCES} ${LG_RESOURCES_OBJS})
set_target_properties(lg_resources PROPERTIES LINKER_LANGUAGE C)
