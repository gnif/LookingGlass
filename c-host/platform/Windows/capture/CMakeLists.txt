cmake_minimum_required(VERSION 3.0)
project(capture LANGUAGES C)

include("PreCapture")

if(NOT DEFINED NVFBC_SDK)
  set(NVFBC_SDK "C:\\Program Files (x86)\\NVIDIA Corporation\\NVIDIA Capture SDK")
endif()
  
if(EXISTS "${NVFBC_SDK}\\inc" AND IS_DIRECTORY "${NVFBC_SDK}\\inc")
  set(USE_NVFBC ON)
endif()

option(USE_NVFBC "Enable NVFBC Support" ${USE_NVFBC})
option(USE_DXGI  "Enable DXGI Support" ON)

if(USE_NVFBC)
  add_capture("NVFBC")
endif()

if(USE_DXGI)
  add_capture("DXGI")
endif()

include("PostCapture")

add_library(capture STATIC ${CAPTURE_C})
target_link_libraries(capture ${CAPTURE_LINK})
