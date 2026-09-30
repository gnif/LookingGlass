set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(MSQUIC_MINGW_TRIPLE x86_64-w64-mingw32 CACHE STRING
  "MinGW-w64 target triple")
set(MSQUIC_MINGW_ROOT "/usr/${MSQUIC_MINGW_TRIPLE}" CACHE PATH
  "MinGW-w64 target root")
set(MSQUIC_CLANG_CL clang-cl CACHE FILEPATH
  "clang-cl used to build the static MsQuic archive")
set(MSQUIC_LLVM_LIB llvm-lib CACHE FILEPATH
  "llvm-lib used to create the static MsQuic archive")
set(MSQUIC_WINDRES "${MSQUIC_MINGW_TRIPLE}-windres" CACHE FILEPATH
  "MinGW-w64 resource compiler")
set(MSQUIC_MINGW_GCC "${MSQUIC_MINGW_TRIPLE}-gcc" CACHE FILEPATH
  "MinGW-w64 compiler used to locate compatibility headers")

if(NOT MSQUIC_CLANG_RESOURCE_DIR)
  execute_process(
    COMMAND "${MSQUIC_CLANG_CL}" /clang:-print-resource-dir
    RESULT_VARIABLE MSQUIC_CLANG_RESOURCE_RESULT
    OUTPUT_VARIABLE MSQUIC_CLANG_RESOURCE_ROOT
    ERROR_VARIABLE MSQUIC_CLANG_RESOURCE_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT MSQUIC_CLANG_RESOURCE_RESULT EQUAL 0)
    message(FATAL_ERROR
      "Unable to locate the clang resource headers: "
      "${MSQUIC_CLANG_RESOURCE_ERROR}")
  endif()

  set(MSQUIC_CLANG_RESOURCE_DIR
    "${MSQUIC_CLANG_RESOURCE_ROOT}/include" CACHE PATH
    "clang resource headers used by the MsQuic cross-build")
endif()

if(NOT MSQUIC_MINGW_GCC_INCLUDE)
  execute_process(
    COMMAND "${MSQUIC_MINGW_GCC}" -print-file-name=include
    RESULT_VARIABLE MSQUIC_MINGW_GCC_INCLUDE_RESULT
    OUTPUT_VARIABLE MSQUIC_MINGW_GCC_INCLUDE
    ERROR_VARIABLE MSQUIC_MINGW_GCC_INCLUDE_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT MSQUIC_MINGW_GCC_INCLUDE_RESULT EQUAL 0)
    message(FATAL_ERROR
      "Unable to locate the MinGW-w64 compiler headers: "
      "${MSQUIC_MINGW_GCC_INCLUDE_ERROR}")
  endif()

  set(MSQUIC_MINGW_GCC_INCLUDE "${MSQUIC_MINGW_GCC_INCLUDE}" CACHE PATH
    "MinGW-w64 compiler headers used by the MsQuic cross-build")
endif()

if(NOT MSQUIC_MINGW_GCC_INCLUDE_FIXED)
  execute_process(
    COMMAND "${MSQUIC_MINGW_GCC}" -print-file-name=include-fixed
    RESULT_VARIABLE MSQUIC_MINGW_GCC_FIXED_RESULT
    OUTPUT_VARIABLE MSQUIC_MINGW_GCC_INCLUDE_FIXED
    ERROR_VARIABLE MSQUIC_MINGW_GCC_FIXED_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT MSQUIC_MINGW_GCC_FIXED_RESULT EQUAL 0)
    message(FATAL_ERROR
      "Unable to locate the fixed MinGW-w64 compiler headers: "
      "${MSQUIC_MINGW_GCC_FIXED_ERROR}")
  endif()

  set(MSQUIC_MINGW_GCC_INCLUDE_FIXED
    "${MSQUIC_MINGW_GCC_INCLUDE_FIXED}" CACHE PATH
    "Fixed MinGW-w64 compiler headers used by the MsQuic cross-build")
endif()

foreach(MSQUIC_REQUIRED_INCLUDE
    "${MSQUIC_MINGW_ROOT}/include"
    "${MSQUIC_CLANG_RESOURCE_DIR}"
    "${MSQUIC_MINGW_GCC_INCLUDE}")
  if(NOT IS_DIRECTORY "${MSQUIC_REQUIRED_INCLUDE}")
    message(FATAL_ERROR
      "Required MsQuic cross-build headers are missing: "
      "${MSQUIC_REQUIRED_INCLUDE}")
  endif()
endforeach()

set(CMAKE_C_COMPILER          "${MSQUIC_CLANG_CL}")
set(CMAKE_CXX_COMPILER        "${MSQUIC_CLANG_CL}")
set(CMAKE_C_COMPILER_TARGET   "${MSQUIC_MINGW_TRIPLE}")
set(CMAKE_CXX_COMPILER_TARGET "${MSQUIC_MINGW_TRIPLE}")
set(CMAKE_AR                  "${MSQUIC_LLVM_LIB}")
set(CMAKE_RC_COMPILER         "${MSQUIC_WINDRES}")

set(MSQUIC_COMPAT_HEADER
  "${CMAKE_CURRENT_LIST_DIR}/msquic-mingw-compat.h")
set(MSQUIC_CLANG_CL_OPTIONS
  "/clang:-nostdinc"
  "/clang:-isystem"
  "/clang:${MSQUIC_MINGW_ROOT}/include"
  "/clang:-isystem"
  "/clang:${MSQUIC_CLANG_RESOURCE_DIR}"
  "/clang:-isystem"
  "/clang:${MSQUIC_MINGW_GCC_INCLUDE}")

if(IS_DIRECTORY "${MSQUIC_MINGW_GCC_INCLUDE_FIXED}")
  list(APPEND MSQUIC_CLANG_CL_OPTIONS
    "/clang:-isystem"
    "/clang:${MSQUIC_MINGW_GCC_INCLUDE_FIXED}")
endif()

list(APPEND MSQUIC_CLANG_CL_OPTIONS
  "/clang:-fms-extensions"
  "/clang:-Wno-unknown-pragmas"
  "/clang:-Wno-microsoft-anon-tag"
  "/clang:-Wno-missing-field-initializers"
  "/clang:-Wno-sign-compare"
  "/clang:-Wno-int-conversion"
  "/clang:-Wno-unused-function"
  "/clang:-Wno-incompatible-pointer-types"
  "/clang:-Wno-duplicate-decl-specifier"
  "/FI${MSQUIC_COMPAT_HEADER}")
list(JOIN MSQUIC_CLANG_CL_OPTIONS " " MSQUIC_CLANG_CL_FLAGS)

set(CMAKE_C_FLAGS "${MSQUIC_CLANG_CL_FLAGS}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${MSQUIC_CLANG_CL_FLAGS}" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_DEBUG "/clang:-O0 /Zi" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_DEBUG "/clang:-O0 /Zi" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELEASE "/clang:-O3 /DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "/clang:-O3 /DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELWITHDEBINFO
  "/clang:-O2 /Zi /DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO
  "/clang:-O2 /Zi /DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_MINSIZEREL "/clang:-Os /DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_MINSIZEREL
  "/clang:-Os /DNDEBUG" CACHE STRING "" FORCE)

set(CMAKE_USER_MAKE_RULES_OVERRIDE
  "${CMAKE_CURRENT_LIST_DIR}/msquic-clangcl-rules.cmake")

set(CMAKE_FIND_ROOT_PATH "${MSQUIC_MINGW_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
