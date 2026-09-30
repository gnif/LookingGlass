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
)

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

target_include_directories(transport_Network PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

target_link_libraries(transport_Network PRIVATE
  lg_common
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
