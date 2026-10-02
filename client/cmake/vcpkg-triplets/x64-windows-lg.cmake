set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE         dynamic)
set(VCPKG_PROVIDED_FORTRAN    ON)

if(PORT STREQUAL "openssl")
  set(VCPKG_LIBRARY_LINKAGE static)
else()
  set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
