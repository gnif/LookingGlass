cmake_minimum_required(VERSION 3.0)
project(fonts LANGUAGES C)

set(FONT_H "${CMAKE_BINARY_DIR}/include/dynamic/fonts.h")
set(FONT_C "${CMAKE_BINARY_DIR}/src/fonts.c")

file(WRITE  ${FONT_H} "#include \"interface/font.h\"\n\n")
file(APPEND ${FONT_H} "extern LG_Font * LG_Fonts[];\n\n")

file(WRITE  ${FONT_C} "#include \"interface/font.h\"\n\n")
file(APPEND ${FONT_C} "#include <stddef.h>\n\n")

set(FONTS "_")
set(FONTS_LINK "_")
function(add_font name)
  set(FONTS      "${FONTS};${name}" PARENT_SCOPE)
  set(FONTS_LINK "${FONTS_LINK};font_${name}" PARENT_SCOPE)
  add_subdirectory(${name})
endfunction()

# Add/remove fonts here!
add_font(freetype)

list(REMOVE_AT FONTS      0)
list(REMOVE_AT FONTS_LINK 0)

list(LENGTH FONTS FONT_COUNT)
file(APPEND ${FONT_H} "#define LG_FONT_COUNT ${FONT_COUNT}\n")

foreach(font ${FONTS})
  file(APPEND ${FONT_C} "extern LG_Font LGF_${font};\n")
endforeach()

file(APPEND ${FONT_C} "\nconst LG_Font * LG_Fonts[] =\n{\n")
foreach(font ${FONTS})
  file(APPEND ${FONT_C} "  &LGF_${font},\n")
endforeach()
file(APPEND ${FONT_C} "  NULL\n};\n\n")

add_library(fonts STATIC ${FONT_C})
target_link_libraries(fonts ${FONTS_LINK})
