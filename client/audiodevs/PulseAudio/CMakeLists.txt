cmake_minimum_required(VERSION 3.0)
project(audiodev_PulseAudio LANGUAGES C)

find_package(PkgConfig)
pkg_check_modules(AUDIODEV_PulseAudio REQUIRED IMPORTED_TARGET
  libpulse
)

add_library(audiodev_PulseAudio STATIC
  pulseaudio.c
)

target_link_libraries(audiodev_PulseAudio
  PkgConfig::AUDIODEV_PulseAudio
  lg_common
)

target_include_directories(audiodev_PulseAudio
  PRIVATE
    src
)
