cmake_minimum_required(VERSION 3.0)
project(decoders LANGUAGES C)

#find_package(PkgConfig)
#pkg_check_modules(DECODERS_PKGCONFIG REQUIRED
#)

add_library(decoders STATIC
	src/null.c
	src/yuv420.c
)

target_link_libraries(decoders
	lg_common
	${DECODERS_PKGCONFIG_LIBRARIES}
)

target_include_directories(decoders
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
	PRIVATE
		src
		${DECODERS_PKGCONFIG_INCLUDE_DIRS}
)
