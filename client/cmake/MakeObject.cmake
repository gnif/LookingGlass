function(make_object out_var)
  set(result)
  set(result_h)
  foreach(in_f ${ARGN})
    file(RELATIVE_PATH out_f ${CMAKE_CURRENT_SOURCE_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/${in_f}")
    set(out_h "${CMAKE_CURRENT_BINARY_DIR}/${out_f}.h")
    set(out_f "${CMAKE_CURRENT_BINARY_DIR}/${out_f}.o")
    string(REGEX REPLACE "[/.]" "_" sym_in  ${in_f})

    add_custom_command(OUTPUT ${out_f}
      COMMAND ${CMAKE_LINKER} -r -b binary -o ${out_f} ${in_f}
      COMMAND ${CMAKE_OBJCOPY} --rename-section .data=.rodata,CONTENTS,ALLOC,LOAD,READONLY,DATA ${out_f} ${out_f}
      COMMAND ${CMAKE_OBJCOPY} --redefine-sym _binary_${sym_in}_start=b_${sym_in} ${out_f} ${out_f}
      COMMAND ${CMAKE_OBJCOPY} --redefine-sym _binary_${sym_in}_end=b_${sym_in}_end ${out_f} ${out_f}
      COMMAND ${CMAKE_OBJCOPY} --strip-symbol _binary_${sym_in}_size ${out_f} ${out_f}
      DEPENDS ${in_f}
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Creating object from ${in_f}"
      VERBATIM
    )

    file(WRITE  ${out_h} "extern const char b_${sym_in}[];\n")
    file(APPEND ${out_h} "extern const char b_${sym_in}_end[];\n")
    file(APPEND ${out_h} "#define b_${sym_in}_size (b_${sym_in}_end - b_${sym_in})\n")

    get_filename_component(h_dir ${out_h} DIRECTORY)
    list(APPEND result_h ${h_dir})
    list(APPEND result ${out_f})
  endforeach()
  list(REMOVE_DUPLICATES result_h)

  set(${out_var}_OBJS "${result}"   PARENT_SCOPE)
  set(${out_var}_INCS "${result_h}" PARENT_SCOPE)
endfunction()
