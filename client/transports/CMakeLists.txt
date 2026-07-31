cmake_minimum_required(VERSION 3.10)
project(transports LANGUAGES C)

set(TRANSPORT_H "${CMAKE_BINARY_DIR}/include/dynamic/transports.h")
set(TRANSPORT_C "${CMAKE_BINARY_DIR}/src/transports.c")

file(WRITE  ${TRANSPORT_H} "#include \"interface/transport.h\"\n\n")
file(APPEND ${TRANSPORT_H} "extern const LG_TransportOps * LG_Transports[];\n\n")

file(WRITE  ${TRANSPORT_C} "#include \"interface/transport.h\"\n\n")
file(APPEND ${TRANSPORT_C} "#include <stddef.h>\n\n")

set(TRANSPORTS "_")
set(TRANSPORTS_LINK "_")
function(add_transport name)
  set(TRANSPORTS      "${TRANSPORTS};${name}" PARENT_SCOPE)
  set(TRANSPORTS_LINK "${TRANSPORTS_LINK};transport_${name}" PARENT_SCOPE)
  add_subdirectory(${name})
endfunction()

# Add/remove transports here!
add_transport(LGMP)
if(ENABLE_TESTS)
  add_transport(Test)
endif()

list(REMOVE_AT TRANSPORTS      0)
list(REMOVE_AT TRANSPORTS_LINK 0)

list(LENGTH TRANSPORTS TRANSPORT_COUNT)
file(APPEND ${TRANSPORT_H} "#define LG_TRANSPORT_COUNT ${TRANSPORT_COUNT}\n")

foreach(transport ${TRANSPORTS})
  file(APPEND ${TRANSPORT_C} "extern const LG_TransportOps LGT_${transport};\n")
endforeach()

file(APPEND ${TRANSPORT_C} "\nconst LG_TransportOps * LG_Transports[] =\n{\n")
foreach(transport ${TRANSPORTS})
  file(APPEND ${TRANSPORT_C} "  &LGT_${transport},\n")
endforeach()
file(APPEND ${TRANSPORT_C} "  NULL\n};")

add_library(transports STATIC ${TRANSPORT_C})
target_link_libraries(transports ${TRANSPORTS_LINK})
