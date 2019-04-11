cmake_minimum_required(VERSION 3.0)
project(spice LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(SPICE_PKGCONFIG REQUIRED
	spice-protocol
	nettle
	hogweed
)

add_definitions(-D USE_NETTLE)

add_library(spice STATIC
	src/spice.c
	src/rsa.c
)

target_link_libraries(spice
	lg_common
	${SPICE_PKGCONFIG_LIBRARIES}
)

target_include_directories(spice
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
	PRIVATE
		src
		${SPICE_PKGCONFIG_INCLUDE_DIRS}
)
