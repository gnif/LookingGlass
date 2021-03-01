# Try to find the BFD librairies
# BFD_FOUND - system has BFD lib
# BFD_INCLUDE_DIR - the BFD include directory
# BFD_LIBRARIES - Libraries needed to use BFD

if (BFD_INCLUDE_DIR AND BFD_LIBRARIES)
  # Already in cache, be silent
  set(BFD_FIND_QUIETLY TRUE)
endif (BFD_INCLUDE_DIR AND BFD_LIBRARIES)

find_path(BFD_INCLUDE_DIR NAMES bfd.h)
find_library(BFD_LIBRARIES NAMES bfd)

include(FindPackageHandleStandardArgs)

if (";${BFD_LIBRARIES};" MATCHES "bfd.a;")
  MESSAGE(STATUS "Linking against static bfd")

  find_library(BFD_LIBIBERTY_LIBRARIES NAMES libiberty.a)
  find_package_handle_standard_args(BFD_LIBIBERTY DEFAULT_MSG BFD_LIBIBERTY_LIBRARIES)

  find_library(BFD_LIBZ_LIBRARIES NAMES libz.a)
  find_package_handle_standard_args(BFD_LIBZ DEFAULT_MSG BFD_LIBZ_LIBRARIES)

  if (NOT ${BFD_LIBIBERTY_FOUND})
    message(FATAL_ERROR "Using static libbfd.a, but libiberty.a not available")
  elseif (NOT ${BFD_LIBZ_FOUND})
    message(FATAL_ERROR "Using static libbfd.a, but libz.a not available")
  else()
    list(APPEND BFD_LIBRARIES ${BFD_LIBIBERTY_LIBRARIES} ${BFD_LIBZ_LIBRARIES})
  endif()
endif()

MESSAGE(STATUS "BFD libs: " "${BFD_LIBRARIES}")

find_package_handle_standard_args(BFD DEFAULT_MSG BFD_LIBRARIES BFD_INCLUDE_DIR)

MESSAGE(STATUS "BFD libs: " "${BFD_LIBRARIES}")

mark_as_advanced(BFD_INCLUDE_DIR BFD_LIBRARIES)
