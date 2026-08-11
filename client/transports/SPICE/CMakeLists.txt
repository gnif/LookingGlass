cmake_minimum_required(VERSION 3.10)
project(transport_SPICE LANGUAGES C)

add_subdirectory(
  "${PROJECT_TOP}/repos/PureSpice"
  "${CMAKE_BINARY_DIR}/PureSpice"
)

set(SOURCES
  clipboard.c
  input.c
  session.c
  spice.c
  surface.c
)

if(ENABLE_AUDIO)
  list(APPEND SOURCES
    audio.c
  )
endif()

if(ENABLE_USB_AUDIO)
  list(APPEND SOURCES
    audio_usb.c
    usbredir.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/usb_audio.c
  )
endif()

add_library(transport_SPICE STATIC ${SOURCES})

target_include_directories(transport_SPICE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

target_link_libraries(transport_SPICE PRIVATE
  lg_common
  purespice
)

if(ENABLE_AUDIO)
  target_compile_definitions(transport_SPICE PRIVATE ENABLE_AUDIO)
endif()

if(ENABLE_USB_AUDIO)
  target_compile_definitions(transport_SPICE PRIVATE ENABLE_USB_AUDIO)
  target_link_libraries(transport_SPICE PRIVATE
    PkgConfig::USBREDIRPARSER
  )
endif()
