cmake_minimum_required(VERSION 3.0)
project(audiodevs LANGUAGES C)

set(AUDIODEV_H "${CMAKE_BINARY_DIR}/include/dynamic/audiodev.h")
set(AUDIODEV_C "${CMAKE_BINARY_DIR}/src/audiodev.c")

file(WRITE  ${AUDIODEV_H} "#include \"interface/audiodev.h\"\n\n")
file(APPEND ${AUDIODEV_H} "extern struct LG_AudioDevOps * LG_AudioDevs[];\n\n")

file(WRITE  ${AUDIODEV_C} "#include \"interface/audiodev.h\"\n\n")
file(APPEND ${AUDIODEV_C} "#include <stddef.h>\n\n")

set(AUDIODEVS "_")
set(AUDIODEVS_LINK "_")
function(add_audiodev name)
  set(AUDIODEVS      "${AUDIODEVS};${name}" PARENT_SCOPE)
  set(AUDIODEVS_LINK "${AUDIODEVS_LINK};audiodev_${name}" PARENT_SCOPE)
  add_subdirectory(${name})
endfunction()

# Add/remove audiodevs here!
if(ENABLE_PIPEWIRE)
  add_audiodev(PipeWire)
endif()
if(ENABLE_PULSEAUDIO)
  add_audiodev(PulseAudio)
endif()

list(REMOVE_AT AUDIODEVS      0)
list(REMOVE_AT AUDIODEVS_LINK 0)

list(LENGTH AUDIODEVS AUDIODEV_COUNT)
file(APPEND ${AUDIODEV_H} "#define LG_AUDIODEV_COUNT ${AUDIODEV_COUNT}\n")

foreach(audiodev ${AUDIODEVS})
  file(APPEND ${AUDIODEV_C} "extern struct LG_AudioDevOps LGAD_${audiodev};\n")
endforeach()

file(APPEND ${AUDIODEV_C} "\nconst struct LG_AudioDevOps * LG_AudioDevs[] =\n{\n")
foreach(audiodev ${AUDIODEVS})
  file(APPEND ${AUDIODEV_C} "  &LGAD_${audiodev},\n")
endforeach()
file(APPEND ${AUDIODEV_C} "  NULL\n};")

add_library(audiodevs STATIC ${AUDIODEV_C})
target_link_libraries(audiodevs ${AUDIODEVS_LINK})
