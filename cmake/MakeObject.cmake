if(LG_MAKE_OBJECT_ACTION STREQUAL "EMBED")
  if(NOT DEFINED LG_MAKE_OBJECT_INPUT OR
      NOT DEFINED LG_MAKE_OBJECT_SOURCE OR
      NOT DEFINED LG_MAKE_OBJECT_HEADER OR
      NOT DEFINED LG_MAKE_OBJECT_SYMBOL)
    message(FATAL_ERROR
      "The embed action requires input, source, header, and symbol")
  endif()

  file(READ "${LG_MAKE_OBJECT_INPUT}" LG_MAKE_OBJECT_DATA HEX)
  set(LG_MAKE_OBJECT_BYTES "")
  string(LENGTH "${LG_MAKE_OBJECT_DATA}" LG_MAKE_OBJECT_LENGTH)
  set(LG_MAKE_OBJECT_OFFSET 0)
  while(LG_MAKE_OBJECT_OFFSET LESS LG_MAKE_OBJECT_LENGTH)
    math(EXPR LG_MAKE_OBJECT_REMAINING
      "${LG_MAKE_OBJECT_LENGTH} - ${LG_MAKE_OBJECT_OFFSET}")
    if(LG_MAKE_OBJECT_REMAINING GREATER 32)
      set(LG_MAKE_OBJECT_CHUNK_LENGTH 32)
    else()
      set(LG_MAKE_OBJECT_CHUNK_LENGTH "${LG_MAKE_OBJECT_REMAINING}")
    endif()

    string(SUBSTRING "${LG_MAKE_OBJECT_DATA}"
      ${LG_MAKE_OBJECT_OFFSET} ${LG_MAKE_OBJECT_CHUNK_LENGTH}
      LG_MAKE_OBJECT_CHUNK)
    string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])"
      "(char)0x\\1, " LG_MAKE_OBJECT_CHUNK "${LG_MAKE_OBJECT_CHUNK}")
    string(APPEND LG_MAKE_OBJECT_BYTES
      "  ${LG_MAKE_OBJECT_CHUNK}\n")
    math(EXPR LG_MAKE_OBJECT_OFFSET
      "${LG_MAKE_OBJECT_OFFSET} + ${LG_MAKE_OBJECT_CHUNK_LENGTH}")
  endwhile()

  get_filename_component(LG_MAKE_OBJECT_SOURCE_DIR
    "${LG_MAKE_OBJECT_SOURCE}" DIRECTORY)
  get_filename_component(LG_MAKE_OBJECT_HEADER_DIR
    "${LG_MAKE_OBJECT_HEADER}" DIRECTORY)
  file(MAKE_DIRECTORY
    "${LG_MAKE_OBJECT_SOURCE_DIR}"
    "${LG_MAKE_OBJECT_HEADER_DIR}")

  file(WRITE "${LG_MAKE_OBJECT_SOURCE}"
    "#include <stddef.h>\n\n"
    "const char   ${LG_MAKE_OBJECT_SYMBOL}[]    =\n"
    "{\n${LG_MAKE_OBJECT_BYTES}  (char)0x00\n};\n\n"
    "const size_t ${LG_MAKE_OBJECT_SYMBOL}_size =\n"
    "  sizeof(${LG_MAKE_OBJECT_SYMBOL});\n")
  file(WRITE "${LG_MAKE_OBJECT_HEADER}"
    "#include <stddef.h>\n\n"
    "extern const char   ${LG_MAKE_OBJECT_SYMBOL}[];\n"
    "extern const size_t ${LG_MAKE_OBJECT_SYMBOL}_size;\n")
  return()
endif()

function(_lg_expand_includes input include_dir output_var)
  file(READ "${input}" content)

  string(REGEX MATCH
    "(^|\n)#include[ \t]*\"([^\"]+)\"[ \t\r]*(\n|$)" include_match
    "${content}")
  while(include_match)
    set(include_prefix "${CMAKE_MATCH_1}")
    set(include_name   "${CMAKE_MATCH_2}")
    set(include_line   "${include_match}")
    set(include_path   "${include_dir}/${include_name}")
    if(NOT EXISTS "${include_path}")
      message(FATAL_ERROR
        "Unable to resolve ${include_name} included by ${input}")
    endif()

    _lg_expand_includes("${include_path}" "${include_dir}" include_content)
    if(NOT "${include_content}" MATCHES "\n$")
      string(APPEND include_content "\n")
    endif()
    string(REPLACE "${include_line}"
      "${include_prefix}${include_content}" content
      "${content}")
    string(REGEX MATCH
      "(^|\n)#include[ \t]*\"([^\"]+)\"[ \t\r]*(\n|$)" include_match
      "${content}")
  endwhile()

  set(${output_var} "${content}" PARENT_SCOPE)
endfunction()

if(LG_MAKE_OBJECT_ACTION STREQUAL "PREPROCESS")
  if(NOT DEFINED LG_MAKE_OBJECT_INPUT OR
      NOT DEFINED LG_MAKE_OBJECT_OUTPUT OR
      NOT DEFINED LG_MAKE_OBJECT_INCLUDE_DIR)
    message(FATAL_ERROR
      "The preprocess action requires input, output, and include directory")
  endif()

  _lg_expand_includes(
    "${LG_MAKE_OBJECT_INPUT}"
    "${LG_MAKE_OBJECT_INCLUDE_DIR}"
    LG_MAKE_OBJECT_DATA)
  get_filename_component(LG_MAKE_OBJECT_OUTPUT_DIR
    "${LG_MAKE_OBJECT_OUTPUT}" DIRECTORY)
  file(MAKE_DIRECTORY "${LG_MAKE_OBJECT_OUTPUT_DIR}")
  file(WRITE "${LG_MAKE_OBJECT_OUTPUT}" "${LG_MAKE_OBJECT_DATA}")
  return()
endif()

if(LG_MAKE_OBJECT_ACTION STREQUAL "DEFINES")
  if(NOT DEFINED LG_MAKE_OBJECT_INPUT OR
      NOT DEFINED LG_MAKE_OBJECT_OUTPUT)
    message(FATAL_ERROR "The defines action requires input and output")
  endif()

  file(STRINGS "${LG_MAKE_OBJECT_INPUT}" LG_MAKE_OBJECT_DEFINES
    REGEX "^#define")
  set(LG_MAKE_OBJECT_DATA "")
  foreach(LG_MAKE_OBJECT_DEFINE IN LISTS LG_MAKE_OBJECT_DEFINES)
    string(APPEND LG_MAKE_OBJECT_DATA "${LG_MAKE_OBJECT_DEFINE}\n")
  endforeach()
  get_filename_component(LG_MAKE_OBJECT_OUTPUT_DIR
    "${LG_MAKE_OBJECT_OUTPUT}" DIRECTORY)
  file(MAKE_DIRECTORY "${LG_MAKE_OBJECT_OUTPUT_DIR}")
  file(WRITE "${LG_MAKE_OBJECT_OUTPUT}" "${LG_MAKE_OBJECT_DATA}")
  return()
endif()

set(LG_MAKE_OBJECT_SCRIPT "${CMAKE_CURRENT_LIST_FILE}")

function(make_object out_var)
  set(result)
  set(result_h)
  foreach(in_f ${ARGN})
    file(RELATIVE_PATH out_f ${CMAKE_CURRENT_SOURCE_DIR}
      "${CMAKE_CURRENT_SOURCE_DIR}/${in_f}")
    set(in_path "${CMAKE_CURRENT_SOURCE_DIR}/${in_f}")
    set(out_h   "${CMAKE_CURRENT_BINARY_DIR}/${out_f}.h")
    set(out_c   "${CMAKE_CURRENT_BINARY_DIR}/${out_f}.c")
    string(REGEX REPLACE "[/.-]" "_" sym_in "${in_f}")

    add_custom_command(
      OUTPUT "${out_c}" "${out_h}"
      COMMAND "${CMAKE_COMMAND}"
        "-DLG_MAKE_OBJECT_ACTION=EMBED"
        "-DLG_MAKE_OBJECT_INPUT=${in_path}"
        "-DLG_MAKE_OBJECT_SOURCE=${out_c}"
        "-DLG_MAKE_OBJECT_HEADER=${out_h}"
        "-DLG_MAKE_OBJECT_SYMBOL=b_${sym_in}"
        -P "${LG_MAKE_OBJECT_SCRIPT}"
      DEPENDS "${in_path}" "${LG_MAKE_OBJECT_SCRIPT}"
      COMMENT "Embedding ${in_f}"
      VERBATIM
    )

    get_filename_component(h_dir "${out_h}" DIRECTORY)
    list(APPEND result_h "${h_dir}")
    list(APPEND result "${out_c}")
  endforeach()
  list(REMOVE_DUPLICATES result_h)

  set(${out_var}_OBJS "${result}"   PARENT_SCOPE)
  set(${out_var}_INCS "${result_h}" PARENT_SCOPE)
endfunction()

function(make_defines in_file out_file)
  file(RELATIVE_PATH rel_f ${CMAKE_CURRENT_SOURCE_DIR} "${in_file}")
  add_custom_command(
    OUTPUT "${out_file}"
    COMMAND "${CMAKE_COMMAND}"
      "-DLG_MAKE_OBJECT_ACTION=DEFINES"
      "-DLG_MAKE_OBJECT_INPUT=${in_file}"
      "-DLG_MAKE_OBJECT_OUTPUT=${out_file}"
      -P "${LG_MAKE_OBJECT_SCRIPT}"
    DEPENDS "${in_file}" "${LG_MAKE_OBJECT_SCRIPT}"
    COMMENT "Extracting definitions from ${rel_f}"
    VERBATIM
  )
endfunction()
