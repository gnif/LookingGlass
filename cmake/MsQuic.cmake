include(ExternalProject)

# MsQuic's static archive flattener must be able to see this imported target
# from outside the directory where FindOpenSSL is invoked.
if(LG_MSQUIC_OPENSSL_CRYPTO_LIBRARY AND NOT TARGET OpenSSL::Crypto)
  add_library(OpenSSL::Crypto UNKNOWN IMPORTED GLOBAL)
  set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LINK_INTERFACE_LANGUAGES "C"
    IMPORTED_LOCATION                 "${LG_MSQUIC_OPENSSL_CRYPTO_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES     "${LG_MSQUIC_OPENSSL_INCLUDE_DIR}"
  )
endif()

function(lg_add_static_msquic)
  if(TARGET msquic::msquic)
    return()
  endif()

  if(CMAKE_VERSION VERSION_LESS 3.20)
    message(FATAL_ERROR
      "The in-tree static MsQuic target requires CMake 3.20 or newer")
  endif()

  set(LG_MSQUIC_SOURCE_DIR "${PROJECT_TOP}/repos/MsQuic")
  set(LG_MSQUIC_BINARY_DIR "${CMAKE_BINARY_DIR}/MsQuic")
  set(LG_MSQUIC_SYSTEM_LIBRARIES
    ws2_32
    schannel
    secur32
    onecore
    wbemuuid
    winmm
    ntdll
    bcrypt
    ncrypt
    crypt32
    iphlpapi
    advapi32
  )
  set(LG_MSQUIC_CMAKE_ARGS
    "-DQUIC_BUILD_SHARED=OFF"
    "-DQUIC_BUILD_TOOLS=OFF"
    "-DQUIC_BUILD_TEST=OFF"
    "-DQUIC_BUILD_PERF=OFF"
    "-DQUIC_ENABLE_LOGGING=OFF"
    "-DQUIC_LINUX_XDP_ENABLED=OFF"
    "-DQUIC_STATIC_LINK_CRT=OFF"
    "-DQUIC_STATIC_LINK_PARTIAL_CRT=OFF"
    "-DQUIC_SOURCE_LINK=OFF"
    "-DQUIC_PDBALTPATH=OFF"
    "-DQUIC_EMBED_GIT_HASH=OFF"
  )

  if(NOT EXISTS "${LG_MSQUIC_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
      "The MsQuic submodule is not initialized; run "
      "git submodule update --init --recursive")
  endif()

  if(WIN32)
    list(APPEND LG_MSQUIC_CMAKE_ARGS
      "-DQUIC_TLS_LIB=schannel")
    set(LG_MSQUIC_RELEASE_LIBRARY
      "${LG_MSQUIC_BINARY_DIR}/bin/Release/msquic.lib")
  endif()

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(NOT EXISTS "${LG_MSQUIC_SOURCE_DIR}/submodules/quictls/Configure")
      message(FATAL_ERROR
        "The MsQuic submodules are not initialized; run "
        "git submodule update --init --recursive")
    endif()

    find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    find_library(LG_MSQUIC_ATOMIC_LIBRARY NAMES atomic libatomic.so.1)
    find_library(LG_MSQUIC_NUMA_LIBRARY NAMES numa libnuma.so.1)

    set(LG_MSQUIC_DEBUG_LIBRARY
      "${LG_MSQUIC_BINARY_DIR}/bin/Debug/libmsquic.a")
    set(LG_MSQUIC_RELEASE_LIBRARY
      "${LG_MSQUIC_BINARY_DIR}/bin/Release/libmsquic.a")
    set(LG_MSQUIC_SYSTEM_LIBRARIES
      OpenSSL::Crypto
      Threads::Threads
      ${CMAKE_DL_LIBS}
    )
    if(LG_MSQUIC_ATOMIC_LIBRARY)
      list(APPEND LG_MSQUIC_SYSTEM_LIBRARIES
        "${LG_MSQUIC_ATOMIC_LIBRARY}")
    endif()
    if(LG_MSQUIC_NUMA_LIBRARY)
      list(APPEND LG_MSQUIC_SYSTEM_LIBRARIES
        "${LG_MSQUIC_NUMA_LIBRARY}")
    endif()

    list(APPEND LG_MSQUIC_CMAKE_ARGS
      "-DQUIC_TLS_LIB=quictls"
      "-DQUIC_USE_SYSTEM_LIBCRYPTO=ON"
      "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
      "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DOPENSSL_CRYPTO_LIBRARY=${OPENSSL_CRYPTO_LIBRARY}"
      "-DOPENSSL_INCLUDE_DIR=${OPENSSL_INCLUDE_DIR}"
      "-DCMAKE_PROJECT_INCLUDE=${PROJECT_TOP}/cmake/MsQuic.cmake"
      "-DLG_MSQUIC_OPENSSL_CRYPTO_LIBRARY=${OPENSSL_CRYPTO_LIBRARY}"
      "-DLG_MSQUIC_OPENSSL_INCLUDE_DIR=${OPENSSL_INCLUDE_DIR}"
    )
    if(CMAKE_TOOLCHAIN_FILE)
      if(IS_ABSOLUTE "${CMAKE_TOOLCHAIN_FILE}")
        set(LG_MSQUIC_TOOLCHAIN_FILE "${CMAKE_TOOLCHAIN_FILE}")
      elseif(EXISTS "${CMAKE_BINARY_DIR}/${CMAKE_TOOLCHAIN_FILE}")
        get_filename_component(LG_MSQUIC_TOOLCHAIN_FILE
          "${CMAKE_TOOLCHAIN_FILE}" ABSOLUTE BASE_DIR "${CMAKE_BINARY_DIR}")
      else()
        get_filename_component(LG_MSQUIC_TOOLCHAIN_FILE
          "${CMAKE_TOOLCHAIN_FILE}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
      endif()
      list(APPEND LG_MSQUIC_CMAKE_ARGS
        "-DCMAKE_TOOLCHAIN_FILE=${LG_MSQUIC_TOOLCHAIN_FILE}")
    endif()
    if(CMAKE_SYSROOT)
      list(APPEND LG_MSQUIC_CMAKE_ARGS
        "-DCMAKE_SYSROOT=${CMAKE_SYSROOT}")
    endif()

    if(CMAKE_CONFIGURATION_TYPES)
      set(LG_MSQUIC_BUILD_COMMAND
        "${CMAKE_COMMAND}" --build <BINARY_DIR>
        --config $<CONFIG>
        --target msquic_lib
        --parallel
      )
    else()
      set(LG_MSQUIC_BUILD_COMMAND
        "${CMAKE_COMMAND}" --build <BINARY_DIR>
        --target msquic_lib
        --parallel
      )
    endif()
  elseif(MSVC)
    set(LG_MSQUIC_DEBUG_LIBRARY
      "${LG_MSQUIC_BINARY_DIR}/bin/Debug/msquic.lib")
    set(LG_MSQUIC_BUILD_CONFIG "$<CONFIG>")
    set(LG_MSQUIC_BUILD_COMMAND
      "${CMAKE_COMMAND}" --build <BINARY_DIR>
      --config ${LG_MSQUIC_BUILD_CONFIG}
      --target msquic_lib
      --parallel
    )
  elseif(MINGW)
    if(NOT MINGW64_TRIPLE)
      set(MINGW64_TRIPLE x86_64-w64-mingw32)
    endif()

    if(CMAKE_FIND_ROOT_PATH)
      list(GET CMAKE_FIND_ROOT_PATH 0 LG_MSQUIC_MINGW_ROOT)
    else()
      set(LG_MSQUIC_MINGW_ROOT "/usr/${MINGW64_TRIPLE}")
    endif()

    find_library(LG_MSQUIC_NTDLL_LIBRARY
      NAMES ntdll
      PATHS "${LG_MSQUIC_MINGW_ROOT}/lib"
      NO_DEFAULT_PATH
      REQUIRED)
    list(REMOVE_ITEM LG_MSQUIC_SYSTEM_LIBRARIES ntdll)
    list(APPEND LG_MSQUIC_SYSTEM_LIBRARIES
      "${LG_MSQUIC_NTDLL_LIBRARY}")

    set(LG_MSQUIC_DEBUG_LIBRARY
      "${LG_MSQUIC_BINARY_DIR}/bin/Release/msquic.lib")
    find_program(LG_MSQUIC_CLANG_CL clang-cl REQUIRED)
    find_program(LG_MSQUIC_LLVM_LIB llvm-lib REQUIRED)
    find_program(LG_MSQUIC_MINGW_GCC
      NAMES "${MINGW64_TRIPLE}-gcc" x86_64-w64-mingw32-gcc
      REQUIRED)
    find_program(LG_MSQUIC_WINDRES
      NAMES "${MINGW64_TRIPLE}-windres" x86_64-w64-mingw32-windres
      REQUIRED)
    find_program(LG_MSQUIC_DLLTOOL
      NAMES "${MINGW64_TRIPLE}-dlltool" x86_64-w64-mingw32-dlltool
      REQUIRED)

    # MsQuic's Windows sources use the CL command-line ABI and its monolithic
    # archive rule uses lib.exe syntax. Compile those sources with clang-cl,
    # while targeting the same MinGW-w64 ABI and import libraries as the
    # surrounding client build.
    set(LG_MSQUIC_IMPORT_DIR "${LG_MSQUIC_BINARY_DIR}/mingw-imports")
    file(MAKE_DIRECTORY "${LG_MSQUIC_IMPORT_DIR}")

    set(LG_MSQUIC_SECUR32EX_DEF
      "${LG_MSQUIC_IMPORT_DIR}/secur32ex.def")
    set(LG_MSQUIC_SECUR32EX_LIBRARY
      "${LG_MSQUIC_IMPORT_DIR}/secur32ex.lib")
    file(WRITE "${LG_MSQUIC_SECUR32EX_DEF}"
      "LIBRARY secur32.dll\n"
      "EXPORTS\n"
      "  QueryContextAttributesExW\n")
    execute_process(
      COMMAND "${LG_MSQUIC_DLLTOOL}"
        --input-def "${LG_MSQUIC_SECUR32EX_DEF}"
        --output-lib "${LG_MSQUIC_SECUR32EX_LIBRARY}"
      RESULT_VARIABLE LG_MSQUIC_DLLTOOL_RESULT
      ERROR_VARIABLE  LG_MSQUIC_DLLTOOL_ERROR)
    if(NOT LG_MSQUIC_DLLTOOL_RESULT EQUAL 0)
      message(FATAL_ERROR
        "Unable to create the extended Secur32 import library: "
        "${LG_MSQUIC_DLLTOOL_ERROR}")
    endif()

    list(APPEND LG_MSQUIC_SYSTEM_LIBRARIES
      "${LG_MSQUIC_SECUR32EX_LIBRARY}")

    foreach(LG_MSQUIC_SYSTEM_LIBRARY
        onecore
        secur32
        wbemuuid
        winmm)
      string(TOUPPER "${LG_MSQUIC_SYSTEM_LIBRARY}"
        LG_MSQUIC_SYSTEM_LIBRARY_UPPER)
      find_library(LG_MSQUIC_${LG_MSQUIC_SYSTEM_LIBRARY_UPPER}_LIBRARY
        NAMES "${LG_MSQUIC_SYSTEM_LIBRARY}"
        PATHS "${LG_MSQUIC_MINGW_ROOT}/lib"
        NO_DEFAULT_PATH
        REQUIRED)
      configure_file(
        "${LG_MSQUIC_${LG_MSQUIC_SYSTEM_LIBRARY_UPPER}_LIBRARY}"
        "${LG_MSQUIC_IMPORT_DIR}/${LG_MSQUIC_SYSTEM_LIBRARY}.lib"
        COPYONLY)
    endforeach()

    list(APPEND LG_MSQUIC_CMAKE_ARGS
      "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
      "-DCMAKE_TOOLCHAIN_FILE=${PROJECT_TOP}/cmake/toolchains/msquic-clangcl-mingw64.cmake"
      "-DMSQUIC_MINGW_TRIPLE=${MINGW64_TRIPLE}"
      "-DMSQUIC_MINGW_ROOT=${LG_MSQUIC_MINGW_ROOT}"
      "-DMSQUIC_CLANG_CL=${LG_MSQUIC_CLANG_CL}"
      "-DMSQUIC_LLVM_LIB=${LG_MSQUIC_LLVM_LIB}"
      "-DMSQUIC_MINGW_GCC=${LG_MSQUIC_MINGW_GCC}"
      "-DMSQUIC_WINDRES=${LG_MSQUIC_WINDRES}"
      "-DHAS_SPECTRE=OFF"
      "-DHAS_GUARDCF=OFF"
    )
    set(LG_MSQUIC_BUILD_COMMAND
      "${CMAKE_COMMAND}" -E env "LIB=${LG_MSQUIC_IMPORT_DIR}"
      "${CMAKE_COMMAND}" --build <BINARY_DIR>
      --target msquic_lib
      --parallel
    )
  else()
    message(FATAL_ERROR
      "Static MsQuic requires Linux, MSVC, or the MinGW-w64 clang-cl "
      "cross-build toolchain")
  endif()

  ExternalProject_Add(lg_msquic_build
    SOURCE_DIR "${LG_MSQUIC_SOURCE_DIR}"
    BINARY_DIR "${LG_MSQUIC_BINARY_DIR}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    INSTALL_COMMAND ""
    CMAKE_ARGS ${LG_MSQUIC_CMAKE_ARGS}
    BUILD_COMMAND ${LG_MSQUIC_BUILD_COMMAND}
    BUILD_BYPRODUCTS
      "${LG_MSQUIC_DEBUG_LIBRARY}"
      "${LG_MSQUIC_RELEASE_LIBRARY}"
  )

  add_library(lg_msquic STATIC IMPORTED GLOBAL)
  set_target_properties(lg_msquic PROPERTIES
    IMPORTED_CONFIGURATIONS            "Debug;Release;RelWithDebInfo;MinSizeRel"
    IMPORTED_LOCATION                  "${LG_MSQUIC_RELEASE_LIBRARY}"
    IMPORTED_LOCATION_DEBUG            "${LG_MSQUIC_DEBUG_LIBRARY}"
    IMPORTED_LOCATION_RELEASE          "${LG_MSQUIC_RELEASE_LIBRARY}"
    IMPORTED_LOCATION_RELWITHDEBINFO   "${LG_MSQUIC_RELEASE_LIBRARY}"
    IMPORTED_LOCATION_MINSIZEREL       "${LG_MSQUIC_RELEASE_LIBRARY}"
    INTERFACE_COMPILE_DEFINITIONS      QUIC_BUILD_STATIC
    INTERFACE_INCLUDE_DIRECTORIES      "${LG_MSQUIC_SOURCE_DIR}/src/inc"
    INTERFACE_LINK_LIBRARIES           "${LG_MSQUIC_SYSTEM_LIBRARIES}"
  )
  add_dependencies(lg_msquic lg_msquic_build)
  add_library(msquic::msquic ALIAS lg_msquic)
endfunction()
