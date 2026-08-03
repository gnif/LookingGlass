cmake_minimum_required(VERSION 3.10)
project(lg_gui LANGUAGES CXX)

add_library(lg_gui STATIC
  cimgui/cimgui.cpp
  cimgui/imgui/imgui.cpp
  cimgui/imgui/imgui_draw.cpp
  cimgui/imgui/imgui_tables.cpp
  cimgui/imgui/imgui_widgets.cpp
  cimplot/cimplot.cpp
  cimplot/implot/implot.cpp
  cimplot/implot/implot_items.cpp
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lg_gui PRIVATE "-fno-fast-math")

  set_source_files_properties(cimgui/cimgui.cpp PROPERTIES
    COMPILE_FLAGS "-Wno-error=unused-function"
  )
  set_source_files_properties(cimplot/cimplot.cpp PROPERTIES
    COMPILE_FLAGS
      "-Wno-error=array-bounds -Wno-error=unused-function"
  )
endif()

target_compile_definitions(lg_gui PUBLIC
  IMGUI_DISABLE_OBSOLETE_FUNCTIONS=1
  IMGUI_USE_WCHAR32
  IMGUI_USER_CONFIG="../cimconfig.h"
)

if(ENABLE_OPENGL)
  target_sources(lg_gui PRIVATE
    cimgui/imgui/backends/imgui_impl_opengl2.cpp
  )
  target_compile_definitions(lg_gui PUBLIC CIMGUI_USE_OPENGL2=1)
endif()

if(ENABLE_EGL)
  set(IMGUI_OPENGL3_BACKEND
    cimgui/imgui/backends/imgui_impl_opengl3.cpp
  )
  target_sources(lg_gui PRIVATE ${IMGUI_OPENGL3_BACKEND})
  set_source_files_properties(${IMGUI_OPENGL3_BACKEND} PROPERTIES
    COMPILE_DEFINITIONS IMGUI_IMPL_OPENGL_ES3
  )
  target_compile_definitions(lg_gui PUBLIC CIMGUI_USE_OPENGL3=1)
endif()

if(WIN32)
  target_compile_definitions(lg_gui PUBLIC
    IMGUI_IMPL_API=extern\t"C"\t__declspec\(dllexport\)
  )
else()
  target_compile_definitions(lg_gui PUBLIC IMGUI_IMPL_API=extern\t"C"\t)
endif()

target_include_directories(lg_gui PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/cimgui"
  "${CMAKE_CURRENT_SOURCE_DIR}/cimgui/imgui"
  "${CMAKE_CURRENT_SOURCE_DIR}/cimplot"
  "${CMAKE_CURRENT_SOURCE_DIR}/cimplot/implot"
)

set_target_properties(lg_gui PROPERTIES
  CXX_STANDARD 11
  CXX_STANDARD_REQUIRED ON
)
