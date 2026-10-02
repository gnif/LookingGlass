if(NOT DEFINED ENV{VSCMD_VER} OR
    NOT "$ENV{VSCMD_ARG_TGT_ARCH}" MATCHES "^(x64|amd64)$" OR
    NOT "$ENV{VSCMD_ARG_HOST_ARCH}" MATCHES "^(x64|amd64)$")
  message(FATAL_ERROR
    "The windows-clangcl preset requires the x64 Native Tools environment; "
    "open an x64 Native Tools command prompt and configure again")
endif()

find_program(LG_WINDOWS_CLANG_CL
  NAMES clang-cl
  PATHS "$ENV{VCINSTALLDIR}/Tools/Llvm/x64/bin"
  NO_DEFAULT_PATH
  REQUIRED)
execute_process(
  COMMAND "${LG_WINDOWS_CLANG_CL}" -print-target-triple
  RESULT_VARIABLE LG_WINDOWS_CLANG_RESULT
  OUTPUT_VARIABLE LG_WINDOWS_CLANG_TARGET
  ERROR_VARIABLE  LG_WINDOWS_CLANG_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT LG_WINDOWS_CLANG_RESULT EQUAL 0 OR
    NOT LG_WINDOWS_CLANG_TARGET MATCHES "^x86_64-")
  message(FATAL_ERROR
    "The x64 Native Tools environment selected an invalid clang-cl: "
    "${LG_WINDOWS_CLANG_ERROR}${LG_WINDOWS_CLANG_TARGET}")
endif()

set(CMAKE_C_COMPILER   "${LG_WINDOWS_CLANG_CL}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${LG_WINDOWS_CLANG_CL}" CACHE FILEPATH "" FORCE)
