cmake_minimum_required(VERSION 3.0)
project(clipboards LANGUAGES C)

set(CLIPBOARD_H "${CMAKE_BINARY_DIR}/include/dynamic/clipboards.h")
set(CLIPBOARD_C "${CMAKE_BINARY_DIR}/src/clipboards.c")

file(WRITE  ${CLIPBOARD_H} "#include \"interface/clipboard.h\"\n\n")
file(APPEND ${CLIPBOARD_H} "extern LG_Clipboard * LG_Clipboards[];\n\n")

file(WRITE  ${CLIPBOARD_C} "#include \"interface/clipboard.h\"\n\n")
file(APPEND ${CLIPBOARD_C} "#include <stddef.h>\n\n")

set(CLIPBOARDS "_")
set(CLIPBOARDS_LINK "_")
function(add_clipboard name)
  set(CLIPBOARDS      "${CLIPBOARDS};${name}" PARENT_SCOPE)
  set(CLIPBOARDS_LINK "${CLIPBOARDS_LINK};clipboard_${name}" PARENT_SCOPE)
  add_subdirectory(${name})
endfunction()

# Add/remove clipboards here!
if (ENABLE_CB_X11)
  add_clipboard(X11)
endif()

list(REMOVE_AT CLIPBOARDS      0)
list(REMOVE_AT CLIPBOARDS_LINK 0)

list(LENGTH CLIPBOARDS CLIPBOARD_COUNT)
file(APPEND ${CLIPBOARD_H} "#define LG_CLIPBOARD_COUNT ${CLIPBOARD_COUNT}\n")

foreach(clipboard ${CLIPBOARDS})
  file(APPEND ${CLIPBOARD_C} "extern LG_Clipboard LGC_${clipboard};\n")
endforeach()

file(APPEND ${CLIPBOARD_C} "\nconst LG_Clipboard * LG_Clipboards[] =\n{\n")
foreach(clipboard ${CLIPBOARDS})
  file(APPEND ${CLIPBOARD_C} "  &LGC_${clipboard},\n")
endforeach()
file(APPEND ${CLIPBOARD_C} "  NULL\n};\n\n")

add_library(clipboards STATIC ${CLIPBOARD_C})
target_link_libraries(clipboards ${CLIPBOARDS_LINK})
