cmake_minimum_required(VERSION 3.0)
project(capture LANGUAGES C)

include(PreCapture)

option(USE_NVFBC "Enable NVFBC Support" OFF)
option(USE_DXGI  "Enable DXGI Support" ON)

if(NOT DEFINED NVFBC_SDK)
  set(NVFBC_SDK "C:/Program Files (x86)/NVIDIA Corporation/NVIDIA Capture SDK")
endif()

file(TO_CMAKE_PATH "${NVFBC_SDK}" nvfbc_sdk)

if(NOT EXISTS "${nvfbc_sdk}/inc" OR NOT IS_DIRECTORY "${nvfbc_sdk}/inc")
  message("Disabling NVFBC support, can't find the SDK headers")
  set(USE_NVFBC OFF)
endif()

if(USE_DXGI)
  add_capture("DXGI")
endif()

if(USE_NVFBC)
  add_capture("NVFBC")
endif()

include("PostCapture")

add_library(capture STATIC ${CAPTURE_C})
target_link_libraries(capture ${CAPTURE_LINK})
