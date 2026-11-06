cmake_minimum_required(VERSION 3.10)
project(transport_LGMP LANGUAGES C)

set(TRANSPORT_LGMP_SOURCES
  clipboard.c
  input.c
  lgmp.c
)

if(WIN32)
  list(APPEND TRANSPORT_LGMP_SOURCES platform/windows.c)
elseif(UNIX)
  list(APPEND TRANSPORT_LGMP_SOURCES platform/linux.c)
else()
  message(FATAL_ERROR "Unsupported LGMP transport platform")
endif()

if(ENABLE_AUDIO)
  list(APPEND TRANSPORT_LGMP_SOURCES audio.c)

  if(MSVC)
    set_source_files_properties(audio.c PROPERTIES
      COMPILE_OPTIONS "/fp:precise"
    )
  elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(audio.c PROPERTIES
      COMPILE_OPTIONS "-fno-fast-math"
    )
  endif()
endif()

add_library(transport_LGMP STATIC ${TRANSPORT_LGMP_SOURCES})

if(ENABLE_AUDIO)
  target_compile_definitions(transport_LGMP PRIVATE ENABLE_AUDIO)
endif()

target_include_directories(transport_LGMP PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

target_link_libraries(transport_LGMP
  lg_common
  lgmp
)
