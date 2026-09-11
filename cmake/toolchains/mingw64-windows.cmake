# the name of the target operating system
set(CMAKE_SYSTEM_NAME Windows)

# which compilers to use for C and C++
set(MINGW64_TRIPLE x86_64-w64-mingw32 CACHE STRING
  "MinGW-w64 target triple")
set(CMAKE_C_COMPILER   "${MINGW64_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${MINGW64_TRIPLE}-g++")
set(CMAKE_RC_COMPILER  "${MINGW64_TRIPLE}-windres")

# here is the target environment located
set(CMAKE_FIND_ROOT_PATH "/usr/${MINGW64_TRIPLE}")

# adjust the default behaviour of the FIND_XXX() commands:
# search headers and libraries in the target environment, search
# programs in the host environment
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
