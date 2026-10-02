.. _build_client_windows:

Build the Windows client
########################

The Windows client uses ``clang-cl`` and the MSVC ABI. ANGLE continues to
provide EGL and OpenGL ES, while MsQuic and the vcpkg-provided OpenSSL are
linked statically.

Requirements
------------

Install the following tools:

- Visual Studio 2022 Build Tools with the Desktop development with C++
  workload, Clang tools for Windows, and a Windows SDK
- CMake 3.24 or newer
- Ninja, with ``ninja.exe`` available on ``PATH``
- Git
- vcpkg

Set ``VCPKG_ROOT`` to the vcpkg checkout. The client manifest installs native
MSVC-ABI builds of ANGLE, libsamplerate, and static OpenSSL automatically.
Vcpkg also manages the Perl and NASM tools used to build OpenSSL.

Ninja can be installed with ``winget install Ninja-build.Ninja``. Open a new
Developer PowerShell after installing it and confirm that ``ninja --version``
works before configuring the client.

Building
--------

Open an x64 Native Tools command prompt. Alternatively, initialize a regular
PowerShell session for an x64 host and target first (replace ``Community``
with the installed Visual Studio edition when necessary). The process-scoped
policy change below applies only to the current PowerShell session:

.. code:: powershell

   Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
   $vsTools = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools'
   & "$vsTools\Launch-VsDevShell.ps1" `
     -Arch amd64 -HostArch amd64 -SkipAutomaticLocation

Then run:

.. code:: powershell

   git submodule update --init --recursive
   cd client
   cmake --fresh --preset windows-clangcl
   cmake --build --preset windows-clangcl-release

The executable is written beneath ``client/build/windows-clangcl``. The
vcpkg toolchain copies the required ANGLE and libsamplerate runtime DLLs
beside it.

The native build deliberately does not consume MinGW ``.a`` or ``.dll.a``
libraries. All statically linked code must use the MSVC ABI.
