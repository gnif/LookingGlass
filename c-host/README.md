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
mkdir LookingGlass/c-host/build
cd LookingGlass/c-host/build
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

## Where is the log?

It is in your user's temp directory:

    %TEMP%\looking-glass-host.txt

For example:

    C:\Users\YourUser\AppData\Local\Temp\looking-glass-host.txt

You can also open it by simply right clicking the tray icon and selecting "Open Log File"

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
