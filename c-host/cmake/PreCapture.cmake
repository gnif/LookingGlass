set(CAPTURE_H "${CMAKE_BINARY_DIR}/include/dynamic/capture.h")
set(CAPTURE_C "${CMAKE_BINARY_DIR}/src/capture.c")

file(WRITE  ${CAPTURE_H} "#include \"interface/capture.h\"\n\n")
file(APPEND ${CAPTURE_H} "extern CaptureInterface * CaptureInterfaces[];\n\n")

file(WRITE  ${CAPTURE_C} "#include \"interface/capture.h\"\n\n")
file(APPEND ${CAPTURE_C} "#include <stddef.h>\n\n")

set(CAPTURE "_")
set(CAPTURE_LINK "_")
function(add_capture name)
  set(CAPTURE      "${CAPTURE};${name}" PARENT_SCOPE)
  set(CAPTURE_LINK "${CAPTURE_LINK};capture_${name}" PARENT_SCOPE)
  add_subdirectory(${name})
endfunction()
