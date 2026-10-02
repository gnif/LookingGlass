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

# MsQuic assumes that every Windows compiler accepts MSVC command-line
# options. Correct its build flags and component targets so the active MinGW
# toolchain can build them directly.
if(LG_MSQUIC_MINGW_COMPAT_HEADER)
  # MsQuic uses BCrypt pseudo-handles and interlocked list operations exposed
  # by the Windows 10 SDK baseline.
  add_compile_definitions(
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00)

  # GCC diagnoses the MSVC-compatible pointer and integer conversions and
  # Windows-style multi-character pool tags used throughout this code.
  add_compile_options(
    "$<$<COMPILE_LANG_AND_ID:C,GNU>:-Wno-incompatible-pointer-types>"
    "$<$<COMPILE_LANG_AND_ID:C,GNU>:-Wno-int-conversion>"
    "$<$<COMPILE_LANG_AND_ID:C,GNU>:-Wno-multichar>")

  function(lg_configure_msquic_mingw_targets)
    foreach(LG_MSQUIC_TARGET
        inc
        warnings
        main_binary_link_args
        core
        msquic_platform)
      if(NOT TARGET ${LG_MSQUIC_TARGET})
        message(FATAL_ERROR
          "Required MsQuic target is missing: ${LG_MSQUIC_TARGET}")
      endif()
    endforeach()

    set_target_properties(
      inc
      warnings
      main_binary_link_args
      PROPERTIES
        INTERFACE_COMPILE_OPTIONS "")

    target_include_directories(msquic_platform SYSTEM PRIVATE
      "${PROJECT_SOURCE_DIR}/submodules/xdp-for-windows/published/external")

    foreach(LG_MSQUIC_TARGET
        core
        msquic_platform)
      target_compile_options(${LG_MSQUIC_TARGET} PRIVATE
        "SHELL:-include \"${LG_MSQUIC_MINGW_COMPAT_HEADER}\""
        -fms-extensions)
    endforeach()
  endfunction()

  cmake_language(DEFER CALL lg_configure_msquic_mingw_targets)
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
    "-DQUIC_LINUX_IOURING_ENABLED=OFF"
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

  if(MSVC)
    if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
      message(FATAL_ERROR
        "The native Windows MsQuic build requires Ninja")
    endif()

    find_program(LG_MSQUIC_CL NAMES cl REQUIRED)

    set(OPENSSL_USE_STATIC_LIBS TRUE)
    find_package(OpenSSL 3.5 REQUIRED COMPONENTS SSL Crypto)

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
      set(LG_MSQUIC_OPENSSL_CONFIG DEBUG)
    else()
      set(LG_MSQUIC_OPENSSL_CONFIG RELEASE)
    endif()
    get_target_property(LG_MSQUIC_OPENSSL_SSL_LIBRARY
      OpenSSL::SSL "IMPORTED_LOCATION_${LG_MSQUIC_OPENSSL_CONFIG}")
    get_target_property(LG_MSQUIC_OPENSSL_CRYPTO_LIBRARY
      OpenSSL::Crypto "IMPORTED_LOCATION_${LG_MSQUIC_OPENSSL_CONFIG}")
    if(NOT EXISTS "${LG_MSQUIC_OPENSSL_SSL_LIBRARY}" OR
        NOT EXISTS "${LG_MSQUIC_OPENSSL_CRYPTO_LIBRARY}")
      message(FATAL_ERROR
        "Static MSVC OpenSSL libraries were not found")
    endif()

    get_filename_component(LG_MSQUIC_OPENSSL_SSL_DIR
      "${LG_MSQUIC_OPENSSL_SSL_LIBRARY}" DIRECTORY)
    get_filename_component(LG_MSQUIC_OPENSSL_CRYPTO_DIR
      "${LG_MSQUIC_OPENSSL_CRYPTO_LIBRARY}" DIRECTORY)
    if(NOT "${LG_MSQUIC_OPENSSL_SSL_DIR}" STREQUAL
        "${LG_MSQUIC_OPENSSL_CRYPTO_DIR}")
      message(FATAL_ERROR
        "The MSVC OpenSSL SSL and Crypto libraries must share a directory")
    endif()

    list(APPEND LG_MSQUIC_CMAKE_ARGS
      "-DQUIC_TLS_LIB=openssl"
      "-DQUIC_USE_EXTERNAL_OPENSSL=ON"
      "-DQUIC_USE_SYSTEM_LIBCRYPTO=OFF"
      "-DQUIC_OPENSSL_INCLUDE_DIR=${OPENSSL_INCLUDE_DIR}"
      "-DQUIC_OPENSSL_LIB_DIR=${LG_MSQUIC_OPENSSL_SSL_DIR}"
      "-DQUIC_OPENSSL_ROOT_DIR="
      "-DLIB_SSL=${LG_MSQUIC_OPENSSL_SSL_LIBRARY}"
      "-DLIB_CRYPTO=${LG_MSQUIC_OPENSSL_CRYPTO_LIBRARY}"
      "-DCMAKE_C_COMPILER=${LG_MSQUIC_CL}"
      "-DCMAKE_CXX_COMPILER=${LG_MSQUIC_CL}")
    if(NOT CMAKE_CONFIGURATION_TYPES)
      list(APPEND LG_MSQUIC_CMAKE_ARGS
        "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
    endif()
    list(APPEND LG_MSQUIC_SYSTEM_LIBRARIES
      gdi32
      user32
      wbemuuid
      winmm)
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
    if(CMAKE_SYSTEM_PROCESSOR)
      set(LG_MSQUIC_SYSTEM_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
    elseif(MINGW64_TRIPLE MATCHES "^x86_64-")
      set(LG_MSQUIC_SYSTEM_PROCESSOR AMD64)
    elseif(MINGW64_TRIPLE MATCHES "^(aarch64|arm64)-")
      set(LG_MSQUIC_SYSTEM_PROCESSOR ARM64)
    elseif(MINGW64_TRIPLE MATCHES "^arm")
      set(LG_MSQUIC_SYSTEM_PROCESSOR ARM)
    elseif(MINGW64_TRIPLE MATCHES "^i[3-6]86-")
      set(LG_MSQUIC_SYSTEM_PROCESSOR x86)
    else()
      message(FATAL_ERROR
        "Unable to determine the MinGW-w64 target processor from "
        "${MINGW64_TRIPLE}")
    endif()

    set(OPENSSL_USE_STATIC_LIBS TRUE)
    find_package(OpenSSL 3.5 REQUIRED COMPONENTS SSL Crypto)

    foreach(LG_MSQUIC_OPENSSL_LIBRARY
        "${OPENSSL_SSL_LIBRARY}"
        "${OPENSSL_CRYPTO_LIBRARY}")
      if(NOT EXISTS "${LG_MSQUIC_OPENSSL_LIBRARY}" OR
          LG_MSQUIC_OPENSSL_LIBRARY MATCHES "\\.dll\\.a$")
        message(FATAL_ERROR
          "Static MinGW OpenSSL libraries are required; install the "
          "mingw-w64 OpenSSL package for the active toolchain")
      endif()
    endforeach()

    get_filename_component(LG_MSQUIC_OPENSSL_SSL_DIR
      "${OPENSSL_SSL_LIBRARY}" DIRECTORY)
    get_filename_component(LG_MSQUIC_OPENSSL_CRYPTO_DIR
      "${OPENSSL_CRYPTO_LIBRARY}" DIRECTORY)
    if(NOT "${LG_MSQUIC_OPENSSL_SSL_DIR}" STREQUAL
        "${LG_MSQUIC_OPENSSL_CRYPTO_DIR}")
      message(FATAL_ERROR
        "The MinGW OpenSSL SSL and Crypto libraries must share a directory")
    endif()

    set(LG_MSQUIC_RELEASE_LIBRARY
      "${LG_MSQUIC_BINARY_DIR}/obj/Release/libcore.a")
    set(LG_MSQUIC_RELEASE_LINK_LIBRARIES
      "${LG_MSQUIC_BINARY_DIR}/obj/Release/libmsquic_platform.a"
      OpenSSL::SSL
      OpenSSL::Crypto)
    set(LG_MSQUIC_DEBUG_LIBRARY
      "${LG_MSQUIC_RELEASE_LIBRARY}")
    set(LG_MSQUIC_DEBUG_LINK_LIBRARIES
      ${LG_MSQUIC_RELEASE_LINK_LIBRARIES})

    if(CMAKE_RC_COMPILER)
      set(LG_MSQUIC_WINDRES "${CMAKE_RC_COMPILER}")
    else()
      find_program(LG_MSQUIC_WINDRES
        NAMES
          "${MINGW64_TRIPLE}-windres"
          x86_64-w64-mingw32-windres
          windres
        REQUIRED)
    endif()
    set(LG_MSQUIC_C_COMPILER_ARGS)
    if(CMAKE_C_COMPILER_TARGET AND
        CMAKE_C_COMPILER_ID MATCHES "Clang")
      list(APPEND LG_MSQUIC_C_COMPILER_ARGS
        "--target=${CMAKE_C_COMPILER_TARGET}")
    endif()

    execute_process(
      COMMAND "${CMAKE_C_COMPILER}"
        ${LG_MSQUIC_C_COMPILER_ARGS}
        -print-file-name=libkernel32.a
      RESULT_VARIABLE LG_MSQUIC_MINGW_ROOT_RESULT
      OUTPUT_VARIABLE LG_MSQUIC_MINGW_LIBRARY
      ERROR_VARIABLE  LG_MSQUIC_MINGW_ROOT_ERROR
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT LG_MSQUIC_MINGW_ROOT_RESULT EQUAL 0 OR
        LG_MSQUIC_MINGW_LIBRARY STREQUAL "libkernel32.a")
      message(FATAL_ERROR
        "Unable to locate the MinGW-w64 target root: "
        "${LG_MSQUIC_MINGW_ROOT_ERROR}")
    endif()
    get_filename_component(LG_MSQUIC_MINGW_LIBRARY
      "${LG_MSQUIC_MINGW_LIBRARY}" REALPATH)
    get_filename_component(LG_MSQUIC_MINGW_LIBRARY_DIR
      "${LG_MSQUIC_MINGW_LIBRARY}" DIRECTORY)
    get_filename_component(LG_MSQUIC_MINGW_ROOT
      "${LG_MSQUIC_MINGW_LIBRARY_DIR}" DIRECTORY)

    list(REMOVE_ITEM LG_MSQUIC_SYSTEM_LIBRARIES
      schannel)
    list(APPEND LG_MSQUIC_SYSTEM_LIBRARIES
      wbemuuid
      winmm
      gdi32)

    list(APPEND LG_MSQUIC_CMAKE_ARGS
      "-DQUIC_TLS_LIB=openssl"
      "-DQUIC_USE_EXTERNAL_OPENSSL=ON"
      "-DQUIC_OPENSSL_INCLUDE_DIR=${OPENSSL_INCLUDE_DIR}"
      "-DQUIC_OPENSSL_LIB_DIR=${LG_MSQUIC_OPENSSL_SSL_DIR}"
      "-DLIB_SSL=${OPENSSL_SSL_LIBRARY}"
      "-DLIB_CRYPTO=${OPENSSL_CRYPTO_LIBRARY}"
      "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
      "-DCMAKE_SYSTEM_NAME=Windows"
      "-DCMAKE_SYSTEM_PROCESSOR=${LG_MSQUIC_SYSTEM_PROCESSOR}"
      "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY"
      "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DCMAKE_AR=${CMAKE_AR}"
      "-DCMAKE_RANLIB=${CMAKE_RANLIB}"
      "-DCMAKE_RC_COMPILER=${LG_MSQUIC_WINDRES}"
      "-DCMAKE_FIND_ROOT_PATH=${LG_MSQUIC_MINGW_ROOT}"
      "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER"
      "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY"
      "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY"
      "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY"
      "-DCMAKE_PROJECT_INCLUDE=${PROJECT_TOP}/cmake/MsQuic.cmake"
      "-DLG_MSQUIC_MINGW_COMPAT_HEADER=${PROJECT_TOP}/cmake/toolchains/msquic-mingw-compat.h"
      "-DHAS_SPECTRE=OFF"
      "-DHAS_GUARDCF=OFF"
    )
    if(CMAKE_C_COMPILER_TARGET)
      list(APPEND LG_MSQUIC_CMAKE_ARGS
        "-DCMAKE_C_COMPILER_TARGET=${CMAKE_C_COMPILER_TARGET}")
    endif()
    if(CMAKE_CXX_COMPILER_TARGET)
      list(APPEND LG_MSQUIC_CMAKE_ARGS
        "-DCMAKE_CXX_COMPILER_TARGET=${CMAKE_CXX_COMPILER_TARGET}")
    endif()
    if(CMAKE_SYSROOT)
      list(APPEND LG_MSQUIC_CMAKE_ARGS
        "-DCMAKE_SYSROOT=${CMAKE_SYSROOT}")
    endif()
    set(LG_MSQUIC_BUILD_COMMAND
      "${CMAKE_COMMAND}" --build <BINARY_DIR>
      --target core msquic_platform
      --parallel
    )
  else()
    message(FATAL_ERROR
      "Static MsQuic requires Linux, MSVC, or a MinGW-w64 toolchain")
  endif()

  set(LG_MSQUIC_BUILD_BYPRODUCTS
    "${LG_MSQUIC_DEBUG_LIBRARY}"
    "${LG_MSQUIC_RELEASE_LIBRARY}")
  foreach(LG_MSQUIC_LIBRARY IN LISTS
      LG_MSQUIC_DEBUG_LINK_LIBRARIES
      LG_MSQUIC_RELEASE_LINK_LIBRARIES)
    if(IS_ABSOLUTE "${LG_MSQUIC_LIBRARY}")
      list(APPEND LG_MSQUIC_BUILD_BYPRODUCTS
        "${LG_MSQUIC_LIBRARY}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES LG_MSQUIC_BUILD_BYPRODUCTS)

  set(LG_MSQUIC_LINK_LIBRARIES)
  foreach(LG_MSQUIC_LIBRARY IN LISTS LG_MSQUIC_DEBUG_LINK_LIBRARIES)
    list(APPEND LG_MSQUIC_LINK_LIBRARIES
      "$<$<CONFIG:Debug>:${LG_MSQUIC_LIBRARY}>")
  endforeach()
  foreach(LG_MSQUIC_LIBRARY IN LISTS LG_MSQUIC_RELEASE_LINK_LIBRARIES)
    list(APPEND LG_MSQUIC_LINK_LIBRARIES
      "$<$<NOT:$<CONFIG:Debug>>:${LG_MSQUIC_LIBRARY}>")
  endforeach()
  list(APPEND LG_MSQUIC_LINK_LIBRARIES
    ${LG_MSQUIC_SYSTEM_LIBRARIES})

  set(LG_MSQUIC_RECONFIGURE_COMMAND
    "${CMAKE_COMMAND}"
    -S <SOURCE_DIR>
    -B <BINARY_DIR>
    ${LG_MSQUIC_CMAKE_ARGS}
  )

  # The submodule, configuration, and injected compatibility files can change
  # without invalidating ExternalProject's configure stamp. Reconfigure before
  # every nested build, then let its dependency tracking update only the
  # affected outputs.
  ExternalProject_Add(lg_msquic_build
    SOURCE_DIR "${LG_MSQUIC_SOURCE_DIR}"
    BINARY_DIR "${LG_MSQUIC_BINARY_DIR}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    INSTALL_COMMAND ""
    CMAKE_ARGS ${LG_MSQUIC_CMAKE_ARGS}
    BUILD_ALWAYS ON
    BUILD_COMMAND
      ${LG_MSQUIC_RECONFIGURE_COMMAND}
      COMMAND ${LG_MSQUIC_BUILD_COMMAND}
    BUILD_BYPRODUCTS ${LG_MSQUIC_BUILD_BYPRODUCTS}
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
    INTERFACE_LINK_LIBRARIES           "${LG_MSQUIC_LINK_LIBRARIES}"
  )
  add_dependencies(lg_msquic lg_msquic_build)
  add_library(msquic::msquic ALIAS lg_msquic)
endfunction()
