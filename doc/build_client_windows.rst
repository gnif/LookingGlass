.. _build_client_windows:

Build the Windows client
########################

The Windows client uses ``clang-cl`` and the MSVC ABI. ANGLE continues to
provide EGL and OpenGL ES, while MsQuic and its bundled OpenSSL are linked
statically.

Requirements
------------

Install the following tools:

- Visual Studio 2022 Build Tools with the Desktop development with C++
  workload, Clang tools for Windows, and a Windows SDK
- CMake 3.21 or newer
- Ninja
- Git
- Perl
- NASM
- vcpkg

Set ``VCPKG_ROOT`` to the vcpkg checkout. The client manifest installs native
MSVC-ABI builds of ANGLE and libsamplerate automatically.

Building
--------

Open an x64 Native Tools command prompt or Developer PowerShell, then run:

.. code:: powershell

   git submodule update --init --recursive
   cd client
   cmake --preset windows-clangcl
   cmake --build --preset windows-clangcl-release

The executable is written beneath ``client/build/windows-clangcl``. The
vcpkg toolchain copies the required ANGLE and libsamplerate runtime DLLs
beside it.

The native build deliberately does not consume MinGW ``.a`` or ``.dll.a``
libraries. All statically linked code must use the MSVC ABI.
