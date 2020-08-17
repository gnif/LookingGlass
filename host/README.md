# General Questions

## What is this?

The Looking Glass Host application for the Guest Virtual Machine.

## What platforms does this support?

Currently only Windows is supported however there is some initial support for Linux at this time.

## How do I build it?

#### For Windows on Windows

1. download and install msys2 x86_64 from http://www.msys2.org/ following the setup instructions provided
3. execute `pacman -Fy` and then `pacman -Sy git make mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake`
4. run "C:\msys64\mingw64.exe"
5. checkout the project
  `git clone https://github.com/gnif/LookingGlass.git`
6. configure the project and build it

```
mkdir LookingGlass/host/build
cd LookingGlass/host/build
cmake -G "MSYS Makefiles" ..
make
```

#### For Linux on Linux

```
mkdir build
cd build
cmake ..
make
```

#### For Windows cross compiling on Linux

```
mkdir build
cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw64.cmake ..
make
```

## Building the Windows installer

Install NSIS compiler
Build the host program, see above sections.
Build installer with `makensis platform/Windows/installer.nsi`
The resulting installer will be at
`platform/Windows/looking-glass-host-setup.exe`

## Where is the log?

It is in your user's temp directory:

    %TEMP%\looking-glass-host.txt

Or if running as a system service it will be located in:

    C:\Windows\Temp\looking-glass-host.txt

You can find out where the file is by right clicking on the tray icon and
selecting "Log File Location"

### High priority capture using DXGI and Secure Desktop (UAC) capture support

By default Windows gives priority to the foreground application for any GPU
work which causes issues with capture if the foreground application is consuming
100% of the available GPU resources. The looking glass host application is able
to increase the kernel GPU thread to realtime priority which fixes this, but in
order to do so it must run as the `SYSTEM` user account. To do this, Looking
Glass needs to run as a service. This can be accomplished by either using the
NSIS installer which will do this for you, or you can use the following command
to Install the service manually:

    looking-glass-host.exe InstallService

To remove the service use the following command:

    looking-glass-host.exe UninstallService

This will also enable the host application to capture the secure desktop which
includes things like the lock screen and UAC prompts.

## Why does this version require Administrator privileges

This is intentional for several reasons.

1. NvFBC requires a system wide hook to correctly obtain the cursor position as NVIDIA decided to not provide this as part of the cursor updates.
2. NvFBC requires administrator level access to enable the interface in the first place. (WIP)
3. DXGI performance can be improved if we have this. (WIP)

## NvFBC (NVIDIA Frame Buffer Capture)

### Why isn't there a build with NvFBC support available.

~~Because NVIDIA have decided to put restrictions on the NvFBC API that simply make it incompatible with the GPL/2 licence. Providing a pre-built binary with NvFBC support would violate the EULA I have agreed to in order to access the NVidia Capture SDK.~~

Either I miss-read the License Agreement or it has been updated, it is now viable to produce a "derived work" from the capture SDK.

> 1.1 License Grant. Subject to the terms of this Agreement, NVIDIA hereby grants you a nonexclusive, non-transferable, worldwide,
revocable, limited, royalty-free, fully paid-up license during the term of this Agreement to:
> (i) install, use and reproduce the Licensed Software delivered by NVIDIA plus make modifications and create derivative
works of the source code and header files delivered by NVIDIA, provided that the software is executed only in hardware products as
specified by NVIDIA in the accompanying documentation (such as release notes) as supported, to develop, test and service your
products (each, a “Customer Product”) that are interoperable with supported hardware products. If the NVIDIA documentation is
silent, the supported hardware consists of certain NVIDIA GPUs; and

To be safe we are still not including the NVIDIA headers in the repository, but I am now providing pre-built binaries with NvFBC support included.

See: https://looking-glass.hostfission.com/downloads

### Why can't I compile NvFBC support into the host

You must download and install the NVidia Capture SDK. Please note that by doing so you will be agreeing to NVIDIA's SDK License agreement.

_-Geoff_
