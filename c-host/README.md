# General Questions

## What is this?

This is an experimental rewrite of the host application in pure C using the MinGW toolchain.

## Why make this?

Several reasons:

1. The client is written in C and I would like to unify the project's language
2. The host is currently hard to build using MinGW and is very Windows specific
3. The host is a jumbled mess of code from all the experimentation going on
4. I would eventually like to be able to port this to run on Linux guests

## Why C and not C++ (or some other language)

Beacuse I like C and for this project believe that C++ is overkill

## When will it be ready?

No idea

## Will it replace the C++ host?

Yes, but only when it is feature complete.

## Why doesn't this use CMake?

It does now...
~~Because win-builds doesn't distribute it, so to make it easy for everyone to compile we do not require it.~~

## How do I build it?

Don't ask if you can't figure it out, this code is the very definition of experiemental and incomplete and should not be in use yet.

Hint:

```
mkdir build
cd build
cmake -G "MSYS Makefiles" ..
make
```

## Where is the log?

It is in your user's temp directory:

    %TEMP%\looking-glass-host.txt

For example:

    C:\Users\YourUser\AppData\Local\Temp\looking-glass-host.txt

## Why does this version require Administrator privileges

This is intentional for several reasons.

1. NvFBC requires a system wide hook to correctly obtain the cursor position as NVIDIA decided to not provide this as part of the cursor updates.
2. NvFBC requires administrator level access to enable the interface in the first place. (WIP)
3. DXGI performance can be improved if we have this. (WIP)

# NvFBC (NVIDIA Frame Buffer Capture)

## Why isn't there a build with NvFBC support available.

Because NVIDIA have decided to put restrictions on the NvFBC API that simply make it incompatible with the GPL/2 licence. Providing a pre-built binary with NvFBC support would violate the EULA I have agreed to in order to access the NVidia Capture SDK.

## Why can't I compile NvFBC support into the host

You must download and install the NVidia Capture SDK. Please note that by doing so you will be agreeing to NVIDIA's SDK License agreement and the binary you produce can not be distributed.

## Can't you just re-write the NvFBC headers?

Technically yes, but since I have already agreed to the SDK License Agreement and seen the headers, I am tainted.

Until someone is able to reverse engineer these headers without prior knowleadge obtained from the SDK, and without agreeing to the NVIDIA SDK License, we can not legally include the headers or release a binary with NvFBC support built in.

_-Geoff_
