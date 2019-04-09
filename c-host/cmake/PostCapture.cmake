list(REMOVE_AT CAPTURE      0)
list(REMOVE_AT CAPTURE_LINK 0)

list(LENGTH CAPTURE CAPTURE_COUNT)
file(APPEND ${CAPTURE_H} "#define LG_CAPTURE_COUNT ${CAPTURE_COUNT}\n")

foreach(renderer ${CAPTURE})
  file(APPEND ${CAPTURE_C} "extern CaptureInterface Capture_${renderer};\n")
endforeach()

file(APPEND ${CAPTURE_C} "\nconst CaptureInterface * CaptureInterfaces[] =\n{\n")
foreach(renderer ${CAPTURE})
  file(APPEND ${CAPTURE_C} "  &Capture_${renderer},\n")
endforeach()
file(APPEND ${CAPTURE_C} "  NULL\n};")
