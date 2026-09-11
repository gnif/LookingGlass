set(SampleRate_ROOT "" CACHE PATH
  "Root of a prebuilt libsamplerate distribution")

if(SampleRate_ROOT)
  set(_SampleRate_HINTS       HINTS "${SampleRate_ROOT}")
  set(_SampleRate_FIND_POLICY NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif()

find_path(SampleRate_INCLUDE_DIR
  NAMES samplerate.h
  ${_SampleRate_HINTS}
  PATH_SUFFIXES include
  ${_SampleRate_FIND_POLICY}
)

find_library(SampleRate_LIBRARY
  NAMES samplerate libsamplerate
  ${_SampleRate_HINTS}
  PATH_SUFFIXES lib lib64
  ${_SampleRate_FIND_POLICY}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SampleRate
  REQUIRED_VARS
    SampleRate_INCLUDE_DIR
    SampleRate_LIBRARY
)

mark_as_advanced(
  SampleRate_INCLUDE_DIR
  SampleRate_LIBRARY
)

if(SampleRate_FOUND AND NOT TARGET SampleRate::samplerate)
  add_library(SampleRate::samplerate UNKNOWN IMPORTED)
  set_target_properties(SampleRate::samplerate PROPERTIES
    IMPORTED_LOCATION             "${SampleRate_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SampleRate_INCLUDE_DIR}"
  )
endif()

unset(_SampleRate_HINTS)
unset(_SampleRate_FIND_POLICY)
