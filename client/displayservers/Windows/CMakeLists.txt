cmake_minimum_required(VERSION 3.10)
project(displayserver_Windows LANGUAGES C)

add_library(displayserver_Windows STATIC
  clipboard.c
  input_event.c
  window_size.c
  windows.c
)

if(MSVC)
  set_source_files_properties(input_event.c PROPERTIES
    COMPILE_OPTIONS "/fp:precise"
  )
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(input_event.c PROPERTIES
    COMPILE_OPTIONS "-fno-fast-math"
  )
endif()

target_compile_definitions(displayserver_Windows PRIVATE
  WINVER=0x0A00
  _WIN32_WINNT=0x0A00
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE
)

target_include_directories(displayserver_Windows
  PRIVATE
    .
    ../../src
)

target_link_libraries(displayserver_Windows
  lg_common
  user32
  gdi32
  ole32
)

if(ENABLE_EGL)
  target_link_libraries(displayserver_Windows
    lg_egl_provider
  )
endif()
