cmake_minimum_required(VERSION 3.0)
project(displayservers LANGUAGES C)

set(DISPLAYSERVER_H "${CMAKE_BINARY_DIR}/include/dynamic/displayservers.h")
set(DISPLAYSERVER_C "${CMAKE_BINARY_DIR}/src/displayservers.c")

file(WRITE  ${DISPLAYSERVER_H} "#include \"interface/displayserver.h\"\n\n")
file(APPEND ${DISPLAYSERVER_H} "extern struct LG_DisplayServerOps * LG_DisplayServers[];\n\n")

file(WRITE  ${DISPLAYSERVER_C} "#include \"interface/displayserver.h\"\n\n")
file(APPEND ${DISPLAYSERVER_C} "#include <stddef.h>\n\n")

set(DISPLAYSERVERS "_")
set(DISPLAYSERVERS_LINK "_")
function(add_displayserver name)
  set(DISPLAYSERVERS      "${DISPLAYSERVERS};${name}" PARENT_SCOPE)
  set(DISPLAYSERVERS_LINK "${DISPLAYSERVERS_LINK};displayserver_${name}" PARENT_SCOPE)
  add_subdirectory(${name})
endfunction()

# SDL must be first as it's the default implementation!
add_displayserver(SDL)

# Add/remove displayservers here!
if (ENABLE_X11)
  add_displayserver(X11)
endif()

if (ENABLE_WAYLAND)
  add_displayserver(Wayland)
endif()

list(REMOVE_AT DISPLAYSERVERS      0)
list(REMOVE_AT DISPLAYSERVERS_LINK 0)

list(LENGTH DISPLAYSERVERS DISPLAYSERVER_COUNT)
file(APPEND ${DISPLAYSERVER_H} "#define LG_DISPLAYSERVER_COUNT ${DISPLAYSERVER_COUNT}\n")

foreach(displayserver ${DISPLAYSERVERS})
  file(APPEND ${DISPLAYSERVER_C} "extern struct LG_DisplayServerOps LGDS_${displayserver};\n")
endforeach()

file(APPEND ${DISPLAYSERVER_C} "\nconst struct LG_DisplayServerOps * LG_DisplayServers[] =\n{\n")
foreach(displayserver ${DISPLAYSERVERS})
  file(APPEND ${DISPLAYSERVER_C} "  &LGDS_${displayserver},\n")
endforeach()
file(APPEND ${DISPLAYSERVER_C} "  NULL\n};")

add_library(displayservers STATIC ${DISPLAYSERVER_C})
target_link_libraries(displayservers ${DISPLAYSERVERS_LINK})
