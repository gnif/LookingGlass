cmake_minimum_required(VERSION 3.10)
project(transport_Network LANGUAGES C)

if(NOT TARGET msquic::msquic)
  find_package(msquic CONFIG REQUIRED)
endif()

set(TRANSPORT_NETWORK_SOURCES
  audio.c
  clipboard.c
  connection.c
  control.c
  cursor.c
  input.c
  known_hosts.c
  network.c
  recovery.c
  video.c
  video/decoder_egl.c
  video/pyrowave.c
)

if(MSVC)
  set_source_files_properties(audio.c PROPERTIES
    COMPILE_OPTIONS "/fp:precise"
  )
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(audio.c PROPERTIES
    COMPILE_OPTIONS "-fno-fast-math"
  )
endif()

if(WIN32)
  list(APPEND TRANSPORT_NETWORK_SOURCES
    platform/windows/auth_crypto.c
    platform/windows/fingerprint.c
    platform/windows/platform.c
  )
else()
  find_package(OpenSSL REQUIRED COMPONENTS Crypto)
  list(APPEND TRANSPORT_NETWORK_SOURCES
    platform/linux/auth_crypto.c
    platform/linux/fingerprint.c
    platform/linux/platform.c
  )
endif()

add_library(transport_Network STATIC ${TRANSPORT_NETWORK_SOURCES})

target_compile_definitions(transport_Network PRIVATE
  QUIC_API_ENABLE_INSECURE_FEATURES=1
)

target_include_directories(transport_Network PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

target_link_libraries(transport_Network PRIVATE
  lg_common
  lg_egl_provider
  msquic::msquic
)

if(WIN32)
  target_link_libraries(transport_Network PRIVATE
    bcrypt
    crypt32
  )
else()
  target_link_libraries(transport_Network PRIVATE
    OpenSSL::Crypto
  )
endif()
