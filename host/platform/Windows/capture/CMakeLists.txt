cmake_minimum_required(VERSION 3.10)
project(capture LANGUAGES C)

include(PreCapture)

option(USE_D12   "Enable DirectX12 Support" ON)
option(USE_DXGI  "Enable DXGI Support" ON)
option(USE_NVFBC "Enable NVFBC Support" OFF)

if(NOT DEFINED NVFBC_SDK)
  set(NVFBC_SDK "C:/Program Files (x86)/NVIDIA Corporation/NVIDIA Capture SDK")
endif()

file(TO_CMAKE_PATH "${NVFBC_SDK}" nvfbc_sdk)

if(NOT EXISTS "${nvfbc_sdk}/inc" OR NOT IS_DIRECTORY "${nvfbc_sdk}/inc")
  message("Disabling NVFBC support, can't find the SDK headers")
  set(USE_NVFBC OFF)
endif()

# NOTE: the order here denotes the default
if(USE_D12)
  add_capture("D12")
endif()

if(USE_DXGI)
  add_capture("DXGI")
endif()

if(USE_NVFBC)
  add_capture("NVFBC")
endif()

add_feature_info(USE_D12   USE_D12   "DirectX12 capture backend.")
add_feature_info(USE_DXGI  USE_DXGI  "DXGI Desktop Duplication capture backend.")
add_feature_info(USE_NVFBC USE_NVFBC "NVFBC capture backend.")

include("PostCapture")

add_library(capture STATIC ${CAPTURE_C})
target_link_libraries(capture ${CAPTURE_LINK})
