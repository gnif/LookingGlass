cmake_minimum_required(VERSION 3.10)
project(looking-glass-obs C)

get_filename_component(PROJECT_TOP "${PROJECT_SOURCE_DIR}/.." ABSOLUTE)
list(APPEND CMAKE_MODULE_PATH "${PROJECT_TOP}/cmake/" "${PROJECT_SOURCE_DIR}/cmake/")

include(CheckSubmodule)
include(GNUInstallDirs)
include(CheckCCompilerFlag)
include(FeatureSummary)

set(OPTIMIZE_FOR_NATIVE_DEFAULT ON)
include(OptimizeForNative) # option(OPTIMIZE_FOR_NATIVE)
include(UninstallTarget)

option(ENABLE_BACKTRACE "Enable backtrace support on crash" ON)
add_feature_info(ENABLE_BACKTRACE ENABLE_BACKTRACE "Backtrace support.")

add_compile_options(
  "-Wall"
  "-Werror"
  "-Wfatal-errors"
  "-ffast-math"
  "-fdata-sections"
  "-ffunction-sections"
  "-fpic"
  "$<$<CONFIG:DEBUG>:-O0;-g3;-ggdb>"
)

set(EXE_FLAGS "-Wl,--gc-sections -z noexecstack")
set(CMAKE_C_STANDARD 11)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(OBS_PLUGIN_DIR 64bit)
else()
  set(OBS_PLUGIN_DIR 32bit)
endif()

if(USER_INSTALL)
  if(DEFINED ENV{XDG_CONFIG_HOME})
    set(OBS_PLUGIN_PREFIX $ENV{XDG_CONFIG_HOME})
  else()
    set(OBS_PLUGIN_PREFIX $ENV{HOME}/.config)
  endif()
  set(OBS_PLUGIN_PREFIX ${OBS_PLUGIN_PREFIX}/obs-studio/plugins)
else()
  set(OBS_PLUGIN_PREFIX ${CMAKE_INSTALL_DATADIR}/obs/obs-plugins)
endif()

include(MakeObject)
function(build_effects header_dir)
  file(GLOB headers "${header_dir}/*.h")
  set(OBS_EFFECT_PROCESSED)
  foreach(effect ${ARGN})
    set(out_f "${CMAKE_CURRENT_BINARY_DIR}/${effect}")
    add_custom_command(OUTPUT "${out_f}"
      COMMAND "${AWK}" -f "${CMAKE_CURRENT_SOURCE_DIR}/glsl.include.awk"
              "${CMAKE_CURRENT_SOURCE_DIR}/${effect}" > "${out_f}"
      MAIN_DEPENDENCY "${CMAKE_CURRENT_SOURCE_DIR}/${effect}"
      DEPENDS ${headers}
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/effect"
      COMMENT "Preprocessing effect ${effect}"
      VERBATIM
    )
  endforeach()

  #  set(CMAKE_CURRENT_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
  make_object(
    OBS_EFFECT
    ${ARGN}
  )

  set(OBS_EFFECT_OBJS "${OBS_EFFECT_OBJS}" PARENT_SCOPE)
  set(OBS_EFFECT_INCS "${OBS_EFFECT_INCS}" PARENT_SCOPE)
endfunction()

build_effects(
  effect
  effect/rgb24.effect
)

add_definitions(-D ATOMIC_LOCKING)

add_custom_command(
  OUTPUT  ${CMAKE_BINARY_DIR}/version.c
    ${CMAKE_BINARY_DIR}/_version.c
  COMMAND ${CMAKE_COMMAND} -D PROJECT_TOP=${PROJECT_TOP} -P
    ${PROJECT_TOP}/version.cmake
)

include_directories(
  ${PROJECT_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}/include
  ${OBS_EFFECT_INCS}
)

link_libraries(
  ${CMAKE_DL_LIBS}
  rt
  m
)

set(SOURCES
  ${CMAKE_BINARY_DIR}/version.c
  main.c
  lg.c
)

add_subdirectory("${PROJECT_TOP}/common"          "${CMAKE_BINARY_DIR}/common")
add_subdirectory("${PROJECT_TOP}/repos/LGMP/lgmp" "${CMAKE_BINARY_DIR}/lgmp"  )

add_library(looking-glass-obs SHARED
  ${SOURCES}
  ${OBS_EFFECT_OBJS}
)
target_link_libraries(looking-glass-obs
  ${EXE_FLAGS}
  lg_common
  lgmp
)

install(TARGETS looking-glass-obs
  LIBRARY DESTINATION ${OBS_PLUGIN_PREFIX}/${CMAKE_PROJECT_NAME}/bin/${OBS_PLUGIN_DIR}
)

feature_summary(WHAT ENABLED_FEATURES DISABLED_FEATURES)
