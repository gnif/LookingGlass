cmake_minimum_required(VERSION 3.0)
project(renderers LANGUAGES C)

set(RENDERER_H "${CMAKE_BINARY_DIR}/include/dynamic/renderers.h")
set(RENDERER_C "${CMAKE_BINARY_DIR}/src/renderers.c")

file(WRITE  ${RENDERER_H} "#include \"interface/renderer.h\"\n\n")
file(APPEND ${RENDERER_H} "extern LG_RendererOps * LG_Renderers[];\n\n")

file(WRITE  ${RENDERER_C} "#include \"interface/renderer.h\"\n\n")
file(APPEND ${RENDERER_C} "#include <stddef.h>\n\n")

set(RENDERERS "_")
set(RENDERERS_LINK "_")
function(add_renderer name)
  set(RENDERERS      "${RENDERERS};${name}" PARENT_SCOPE)
  set(RENDERERS_LINK "${RENDERERS_LINK};renderer_${name}" PARENT_SCOPE)
  add_subdirectory(${name})
endfunction()

# Add/remove renderers here!
if(ENABLE_EGL)
  add_renderer(EGL)
endif()
if (ENABLE_OPENGL)
  add_renderer(OpenGL)
endif()

list(REMOVE_AT RENDERERS      0)
list(REMOVE_AT RENDERERS_LINK 0)

list(LENGTH RENDERERS RENDERER_COUNT)
file(APPEND ${RENDERER_H} "#define LG_RENDERER_COUNT ${RENDERER_COUNT}\n")

foreach(renderer ${RENDERERS})
  file(APPEND ${RENDERER_C} "extern LG_RendererOps LGR_${renderer};\n")
endforeach()

file(APPEND ${RENDERER_C} "\nconst LG_RendererOps * LG_Renderers[] =\n{\n")
foreach(renderer ${RENDERERS})
  file(APPEND ${RENDERER_C} "  &LGR_${renderer},\n")
endforeach()
file(APPEND ${RENDERER_C} "  NULL\n};")

add_library(renderers STATIC ${RENDERER_C})
target_link_libraries(renderers ${RENDERERS_LINK})
