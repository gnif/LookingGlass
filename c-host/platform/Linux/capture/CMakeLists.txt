cmake_minimum_required(VERSION 3.0)
project(capture LANGUAGES C)

include("PreCapture")

add_capture("XCB")

include("PostCapture")

add_library(capture STATIC ${CAPTURE_C})
target_link_libraries(capture ${CAPTURE_LINK})
