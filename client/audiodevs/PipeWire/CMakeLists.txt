cmake_minimum_required(VERSION 3.0)
project(audiodev_PipeWire LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(AUDIODEV_PipeWire REQUIRED IMPORTED_TARGET
  libpipewire-0.3
)

add_library(audiodev_PipeWire STATIC
  pipewire.c
)

target_link_libraries(audiodev_PipeWire
  PkgConfig::AUDIODEV_PipeWire
  lg_common
)

target_include_directories(audiodev_PipeWire
  PRIVATE
    src
)
