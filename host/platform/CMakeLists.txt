cmake_minimum_required(VERSION 3.0)
project(platform LANGUAGES C)

if (UNIX)
  set(PLATFORM "Linux")
elseif(WIN32)
  set(PLATFORM "Windows")
endif()

add_subdirectory(${PLATFORM})

add_library(platform INTERFACE)
target_link_libraries(platform INTERFACE platform_${PLATFORM})
