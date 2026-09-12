cmake_minimum_required(VERSION 3.10)
project(looking-glass-client C CXX)

get_filename_component(PROJECT_TOP "${PROJECT_SOURCE_DIR}/.." ABSOLUTE)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR
    "\n"
    "In-source builds are not supported\n"
    "See build instructions provided in: "
    "${PROJECT_TOP}/doc/build.rst\n"
    "Refusing to continue"
  )
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_TOP}/cmake/" "${PROJECT_SOURCE_DIR}/cmake/")

include(CheckSubmodule)
include(GNUInstallDirs)
include(CheckCCompilerFlag)
include(CMakeDependentOption)
include(FeatureSummary)

if(WIN32)
  set(LG_CLIENT_PLATFORM         Windows)
  set(LG_CLIENT_PLATFORM_LINUX   OFF)
  set(LG_CLIENT_PLATFORM_WINDOWS ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(LG_CLIENT_PLATFORM         Linux)
  set(LG_CLIENT_PLATFORM_LINUX   ON)
  set(LG_CLIENT_PLATFORM_WINDOWS OFF)
else()
  message(FATAL_ERROR
    "The Looking Glass client does not support ${CMAKE_SYSTEM_NAME}")
endif()

set(OPTIMIZE_FOR_NATIVE_DEFAULT ON)
include(OptimizeForNative) # option(OPTIMIZE_FOR_NATIVE)
include(UninstallTarget)

if(LG_CLIENT_PLATFORM_LINUX)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
  pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3>=3.10)
endif()

option(ENABLE_OPENGL "Enable the OpenGL renderer"
  ${LG_CLIENT_PLATFORM_LINUX})
add_feature_info(ENABLE_OPENGL ENABLE_OPENGL "Legacy OpenGL renderer.")

option(ENABLE_EGL "Enable the EGL renderer" ON)
add_feature_info(ENABLE_EGL ENABLE_EGL "EGL renderer.")

if(LG_CLIENT_PLATFORM_WINDOWS AND ENABLE_OPENGL)
  message(FATAL_ERROR
    "The legacy OpenGL renderer is not available on Windows; use EGL")
endif()

option(ENABLE_TESTS "Build all client tests and test support" OFF)
add_feature_info(ENABLE_TESTS ENABLE_TESTS
  "Client unit and integration tests with synthetic transport support.")
if(ENABLE_TESTS)
  if(NOT ENABLE_EGL)
    message(FATAL_ERROR "ENABLE_TESTS requires ENABLE_EGL")
  endif()
  add_definitions(-D ENABLE_TESTS)
endif()

option(ENABLE_BACKTRACE "Enable backtrace support on crash" ON)
add_feature_info(ENABLE_BACKTRACE ENABLE_BACKTRACE "Backtrace support.")

option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
add_feature_info(ENABLE_ASAN ENABLE_ASAN "AddressSanitizer support.")

option(ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
add_feature_info(ENABLE_UBSAN ENABLE_UBSAN "UndefinedBehaviorSanitizer support.")

option(ENABLE_AUDIO "Build with audio support" ON)

cmake_dependent_option(
  ENABLE_PIPEWIRE
  "Build with PipeWire audio output support"
  ${LG_CLIENT_PLATFORM_LINUX}
  "ENABLE_AUDIO;LG_CLIENT_PLATFORM_LINUX"
  OFF
)
add_feature_info(ENABLE_PIPEWIRE ENABLE_PIPEWIRE "PipeWire audio support.")

cmake_dependent_option(
  ENABLE_PULSEAUDIO
  "Build with PulseAudio audio output support"
  ${LG_CLIENT_PLATFORM_LINUX}
  "ENABLE_AUDIO;LG_CLIENT_PLATFORM_LINUX"
  OFF
)
add_feature_info(ENABLE_PULSEAUDIO ENABLE_PULSEAUDIO "PulseAudio audio support.")

cmake_dependent_option(
  ENABLE_WASAPI
  "Build with WASAPI audio support"
  ${LG_CLIENT_PLATFORM_WINDOWS}
  "ENABLE_AUDIO;LG_CLIENT_PLATFORM_WINDOWS"
  OFF
)
add_feature_info(ENABLE_WASAPI ENABLE_WASAPI "WASAPI audio support.")

if(ENABLE_AUDIO AND NOT
    (ENABLE_PIPEWIRE OR ENABLE_PULSEAUDIO OR ENABLE_WASAPI))
  message(STATUS "No audio backends enabled, disabling audio support")
  set(ENABLE_AUDIO OFF)
endif()
add_feature_info(ENABLE_AUDIO ENABLE_AUDIO "Audio support.")

option(ENABLE_SPICE "Build with SPICE transport support"
  ${LG_CLIENT_PLATFORM_LINUX})
add_feature_info(ENABLE_SPICE ENABLE_SPICE "SPICE transport support.")

cmake_dependent_option(
  ENABLE_USB_AUDIO
  "Build with USB audio support"
  ${LG_CLIENT_PLATFORM_LINUX}
  "ENABLE_AUDIO;ENABLE_SPICE;LG_CLIENT_PLATFORM_LINUX"
  OFF
)

if(ENABLE_USB_AUDIO)
  pkg_check_modules(USBREDIRPARSER QUIET IMPORTED_TARGET
    libusbredirparser-0.5>=0.7.1)
  if(NOT USBREDIRPARSER_FOUND)
    message(STATUS
      "libusbredirparser was not found, disabling USB audio support")
    set(ENABLE_USB_AUDIO OFF)
  endif()
endif()
add_feature_info(ENABLE_USB_AUDIO ENABLE_USB_AUDIO "USB audio support.")

add_compile_options(
  "-Wall"
  "-Wextra"
  "-Wno-sign-compare"
  "-Wno-unused-parameter"
  "$<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>"
  "$<$<C_COMPILER_ID:GNU>:-Wimplicit-fallthrough=2>"
  "-Werror"
  "-Wfatal-errors"
  "-g"
  "-ffast-math"
  "-fdata-sections"
  "-ffunction-sections"
  "$<$<CONFIG:DEBUG>:-O0;-g3;-ggdb>"
)

if(ENABLE_BACKTRACE)
  add_compile_options(
    "-fno-omit-frame-pointer"
    "-fno-optimize-sibling-calls"
  )
endif()

set(EXE_FLAGS "-Wl,--gc-sections -z noexecstack")
set(CMAKE_C_STANDARD 11)

if(LG_CLIENT_PLATFORM_WINDOWS)
  set(EXE_FLAGS "-Wl,--gc-sections -Wl,--nxcompat")
  add_compile_options("-fdebug-prefix-map=${PROJECT_TOP}/=")
  add_compile_definitions(__USE_MINGW_ANSI_STDIO=0)
endif()

if (ENABLE_OPENGL)
  add_definitions(-D ENABLE_OPENGL)
endif()

if (ENABLE_EGL)
  add_definitions(-D ENABLE_EGL)
endif()

if(ENABLE_ASAN)
  add_compile_options("-fno-omit-frame-pointer" "-fsanitize=address")
  set(EXE_FLAGS "${EXE_FLAGS} -fno-omit-frame-pointer -fsanitize=address")
endif()

if(ENABLE_UBSAN)
  add_compile_options("-fsanitize=undefined")
  set(EXE_FLAGS "${EXE_FLAGS} -fsanitize=undefined")
endif()

add_definitions(-D ATOMIC_LOCKING)
add_definitions(-D GL_GLEXT_PROTOTYPES)

add_custom_command(
  OUTPUT  ${CMAKE_BINARY_DIR}/version.c
    ${CMAKE_BINARY_DIR}/include/version.h
    ${CMAKE_BINARY_DIR}/_version.c
  COMMAND ${CMAKE_COMMAND} -D PROJECT_TOP=${PROJECT_TOP} -P
    ${PROJECT_TOP}/version.cmake
)

include_directories(
  ${PROJECT_TOP}
  ${PROJECT_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}
  ${CMAKE_BINARY_DIR}/include
  ${PROJECT_TOP}/repos/nanosvg/src
)

link_libraries(
  ${CMAKE_DL_LIBS}
)

if(LG_CLIENT_PLATFORM_LINUX)
  link_libraries(
    rt
    m
  )
endif()

set(SOURCES
  ${CMAKE_BINARY_DIR}/version.c
  platform/${LG_CLIENT_PLATFORM}/entry.c
  src/main.c
  src/core.c
  src/app.c
  src/message.c
  src/config.c
  src/keybind.c
  src/font.c
  src/util.c
  src/clipboard.c
  src/kb.c
  src/kb_hid.c
  src/eglutil.c
  src/frame_scheduler.c
  src/overlay_utils.c
  src/render_queue.c
  src/transport.c
  src/fallback.c
  src/sw_surface.c
  src/input.c

  src/overlay/splash.c
  src/overlay/alert.c
  src/overlay/fps.c
  src/overlay/graphs.c
  src/overlay/help.c
  src/overlay/config.c
  src/overlay/msg.c
  src/overlay/status.c
)

if(ENABLE_OPENGL)
  list(APPEND SOURCES src/gl_dynprocs.c)
endif()

if(ENABLE_EGL)
  list(APPEND SOURCES src/egl_dynprocs.c)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(src/overlay/graphs.c PROPERTIES
    COMPILE_FLAGS "-fno-fast-math"
  )
endif()

if(ENABLE_AUDIO)
  list(APPEND SOURCES
    src/audio.c
  )
endif()

add_subdirectory("${PROJECT_TOP}/resources"       "${CMAKE_BINARY_DIR}/resources")
add_subdirectory("${PROJECT_TOP}/common"          "${CMAKE_BINARY_DIR}/common"   )
add_subdirectory("${PROJECT_TOP}/repos/LGMP/lgmp" "${CMAKE_BINARY_DIR}/LGMP"     )

set(LG_EGL_PROVIDER_TARGET "" CACHE STRING
  "CMake target that provides EGL and OpenGL ES")

if(ENABLE_EGL)
  set(LG_EGL_PROVIDER_LIBRARIES)
  if(LG_EGL_PROVIDER_TARGET)
    if(NOT TARGET ${LG_EGL_PROVIDER_TARGET})
      message(FATAL_ERROR
        "LG_EGL_PROVIDER_TARGET does not name an existing CMake target: "
        "${LG_EGL_PROVIDER_TARGET}")
    endif()

    list(APPEND LG_EGL_PROVIDER_LIBRARIES ${LG_EGL_PROVIDER_TARGET})
  elseif(LG_CLIENT_PLATFORM_WINDOWS)
    find_package(ANGLE REQUIRED)
    list(APPEND LG_EGL_PROVIDER_LIBRARIES
      ANGLE::EGL
      ANGLE::GLESv2
    )
  else()
    pkg_check_modules(RENDERER_EGL REQUIRED IMPORTED_TARGET
      egl
      gl
    )
    pkg_check_modules(RENDERER_EGL_OPT IMPORTED_TARGET
      wayland-egl
    )

    list(APPEND LG_EGL_PROVIDER_LIBRARIES PkgConfig::RENDERER_EGL)
    if(RENDERER_EGL_OPT_FOUND)
      list(APPEND LG_EGL_PROVIDER_LIBRARIES PkgConfig::RENDERER_EGL_OPT)
    endif()
  endif()

  add_library(lg_egl_provider INTERFACE)
  target_link_libraries(lg_egl_provider INTERFACE
    ${LG_EGL_PROVIDER_LIBRARIES}
  )
endif()

add_subdirectory("${PROJECT_TOP}/repos/gui" "${CMAKE_BINARY_DIR}/gui"
  EXCLUDE_FROM_ALL)

get_target_property(LG_GUI_INTERFACE_COMPILE_DEFINITIONS
  lg_gui INTERFACE_COMPILE_DEFINITIONS)
if(LG_GUI_INTERFACE_COMPILE_DEFINITIONS)
  list(FILTER LG_GUI_INTERFACE_COMPILE_DEFINITIONS EXCLUDE
    REGEX "^IMGUI_IMPL_API=")
  set_property(TARGET lg_gui PROPERTY INTERFACE_COMPILE_DEFINITIONS
    ${LG_GUI_INTERFACE_COMPILE_DEFINITIONS})
endif()
unset(LG_GUI_INTERFACE_COMPILE_DEFINITIONS)

target_sources(lg_gui PRIVATE
  "${PROJECT_TOP}/repos/gui/cimgui/imgui/imgui_demo.cpp"
  "${PROJECT_TOP}/repos/gui/cimplot/implot/implot_demo.cpp"
)
target_compile_definitions(lg_gui PRIVATE IMGUI_DISABLE_DEMO_WINDOWS)

if(ENABLE_EGL)
  # CMake 3.10 does not allow target_link_libraries to update a target from
  # another directory. LINK_LIBRARIES provides the equivalent dependency.
  set_property(TARGET lg_gui APPEND PROPERTY
    LINK_LIBRARIES lg_egl_provider)
endif()

add_subdirectory(platform)
add_subdirectory(displayservers)
add_subdirectory(renderers)
add_subdirectory(transports)
add_subdirectory(audiodevs)

if(LG_CLIENT_PLATFORM_WINDOWS)
  set(CLIENT_RESOURCE "platform/Windows/resource.rc")
  set_source_files_properties(${CLIENT_RESOURCE} PROPERTIES
    OBJECT_DEPENDS
      "${PROJECT_SOURCE_DIR}/platform/Windows/app.manifest;${PROJECT_TOP}/resources/icon.ico;${CMAKE_BINARY_DIR}/include/version.h"
  )
  add_executable(looking-glass-client WIN32 ${CLIENT_RESOURCE} ${SOURCES})
else()
  configure_file(
    "${PROJECT_TOP}/resources/looking-glass-client.desktop.in"
    "${CMAKE_BINARY_DIR}/resources/looking-glass-client.desktop"
    @ONLY
  )
  add_executable(looking-glass-client ${SOURCES})
endif()

target_compile_definitions(looking-glass-client PRIVATE CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1)

if(LG_CLIENT_PLATFORM_LINUX)
  if(NOT CMAKE_OBJCOPY)
    message(FATAL_ERROR "objcopy is required to generate the client debug file")
  endif()

  if(NOT CMAKE_STRIP)
    message(FATAL_ERROR "strip is required to generate the client debug file")
  endif()

  set(CLIENT_DEBUG_FILE
    "${CMAKE_CURRENT_BINARY_DIR}/looking-glass-client.debug")

  add_custom_command(TARGET looking-glass-client POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" --only-keep-debug
      "$<TARGET_FILE:looking-glass-client>" "${CLIENT_DEBUG_FILE}"
    COMMAND "${CMAKE_STRIP}" --strip-debug
      "$<TARGET_FILE:looking-glass-client>"
    COMMAND "${CMAKE_OBJCOPY}" "--add-gnu-debuglink=${CLIENT_DEBUG_FILE}"
      "$<TARGET_FILE:looking-glass-client>"
    COMMENT "Extracting Looking Glass client debug information"
    VERBATIM
  )
endif()

target_link_libraries(looking-glass-client
  lg_resources
  lg_common
  client_platform
  displayservers
  renderers
  transports
  audiodevs
  lg_gui
)

set_target_properties(looking-glass-client PROPERTIES LINK_FLAGS "${EXE_FLAGS}")

if(ENABLE_TESTS AND LG_CLIENT_PLATFORM_WINDOWS)
  add_library(client_test_version OBJECT
    "${CMAKE_BINARY_DIR}/version.c"
  )
endif()

if(ENABLE_AUDIO)
  if(LG_CLIENT_PLATFORM_WINDOWS)
    find_package(SampleRate REQUIRED)
    set(SAMPLERATE_TARGET SampleRate::samplerate)
  else()
    pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)
    set(SAMPLERATE_TARGET PkgConfig::SAMPLERATE)
  endif()
endif()

if(ENABLE_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()

if(ENABLE_AUDIO)
  target_compile_definitions(looking-glass-client PRIVATE ENABLE_AUDIO)
  target_link_libraries(looking-glass-client
    ${SAMPLERATE_TARGET}
  )
endif()

install(TARGETS looking-glass-client
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  COMPONENT binary)

if(LG_CLIENT_PLATFORM_LINUX)
  install(FILES "${CLIENT_DEBUG_FILE}"
    DESTINATION "${CMAKE_INSTALL_BINDIR}/.debug"
    COMPONENT binary)

  install(FILES "${CMAKE_BINARY_DIR}/resources/looking-glass-client.desktop"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
  install(FILES "${PROJECT_TOP}/resources/lg-logo.svg"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps"
    RENAME "looking-glass.svg")
endif()

feature_summary(WHAT ENABLED_FEATURES DISABLED_FEATURES)
