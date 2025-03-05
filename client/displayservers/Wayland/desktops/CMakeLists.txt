cmake_minimum_required(VERSION 3.10)
project(wayland_desktops LANGUAGES C)

set(DESKTOP_H "${CMAKE_BINARY_DIR}/include/dynamic/wayland_desktops.h")
set(DESKTOP_C "${CMAKE_BINARY_DIR}/src/wayland_desktops.c")

file(WRITE  ${DESKTOP_H} "#include \"interface/desktop.h\"\n\n")
file(APPEND ${DESKTOP_H} "extern struct WL_DesktopOps * WL_Desktops[];\n\n")

file(WRITE  ${DESKTOP_C} "#include \"interface/desktop.h\"\n\n")
file(APPEND ${DESKTOP_C} "#include <stddef.h>\n\n")

set(DESKTOPS "_")
set(DESKTOPS_LINK "_")
function(add_desktop name)
  set(DESKTOPS      "${DESKTOPS};${name}" PARENT_SCOPE)
  set(DESKTOPS_LINK "${DESKTOPS_LINK};wayland_desktop_${name}" PARENT_SCOPE)
  add_subdirectory(${name})
endfunction()

# Add/remove desktops here!

# the first entry here is the default
add_desktop(xdg)

pkg_check_modules(LIBDECOR IMPORTED_TARGET libdecor-0)
if(LIBDECOR_FOUND)
  option(ENABLE_LIBDECOR "Build with libdecor support" ON)
else()
  option(ENABLE_LIBDECOR "Build with libdecor support" OFF)
endif()
add_feature_info(ENABLE_LIBDECOR ENABLE_LIBDECOR "libdecor support.")
if (ENABLE_LIBDECOR)
  add_desktop(libdecor)
endif()

list(REMOVE_AT DESKTOPS      0)
list(REMOVE_AT DESKTOPS_LINK 0)

list(LENGTH DESKTOPS DESKTOP_COUNT)
file(APPEND ${DESKTOP_H} "#define WL_DESKTOP_COUNT ${DESKTOP_COUNT}\n")

foreach(desktop ${DESKTOPS})
  file(APPEND ${DESKTOP_C} "extern struct WL_DesktopOps WLD_${desktop};\n")
endforeach()

file(APPEND ${DESKTOP_C} "\nconst struct WL_DesktopOps * WL_Desktops[] =\n{\n")
foreach(desktop ${DESKTOPS})
  file(APPEND ${DESKTOP_C} "  &WLD_${desktop},\n")
endforeach()
file(APPEND ${DESKTOP_C} "  NULL\n};")

add_library(wayland_desktops STATIC ${DESKTOP_C})
target_link_libraries(wayland_desktops ${DESKTOPS_LINK})
target_include_directories(wayland_desktops
  PRIVATE
    ../
)
