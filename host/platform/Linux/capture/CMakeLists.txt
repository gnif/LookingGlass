cmake_minimum_required(VERSION 3.0)
project(capture LANGUAGES C)

include("PreCapture")

option(USE_XCB "Enable XSHM Support" ON)
option(USE_PIPEWIRE "Enable PipeWire Support" ON)

if (USE_XCB)
  add_capture("XCB")
endif()

if (USE_PIPEWIRE)
  add_capture("pipewire")
endif()

add_feature_info(USE_XCB USE_XCB "XCB/XSHM capture backend.")
add_feature_info(USE_PIPEWIRE USE_PIPEWIRE "Pipewire Screencast capture backend.")

include("PostCapture")

add_library(capture STATIC ${CAPTURE_C})
target_link_libraries(capture ${CAPTURE_LINK})
