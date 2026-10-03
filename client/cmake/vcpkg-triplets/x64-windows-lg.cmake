set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE         dynamic)
set(VCPKG_PROVIDED_FORTRAN    ON)

if(PORT STREQUAL "openssl")
  set(VCPKG_LIBRARY_LINKAGE static)
else()
  set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()

if(PORT STREQUAL "angle")
  # ANGLE's D3D state cache is shared across contexts. Serialize access and
  # dirty cached state whenever calls switch between shared contexts.
  string(CONCAT ANGLE_CONTEXT_FLAGS
    " /DANGLE_ENABLE_SHARE_CONTEXT_LOCK=1"
    " /DANGLE_FORCE_CONTEXT_CHECK_EVERY_CALL=1")
  string(APPEND VCPKG_C_FLAGS   "${ANGLE_CONTEXT_FLAGS}")
  string(APPEND VCPKG_CXX_FLAGS "${ANGLE_CONTEXT_FLAGS}")
endif()
