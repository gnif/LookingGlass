# What is this?

This is an experimental rewrite of the host application in pure C using the MinGW toolchain.

# Why make this?

Several reasons:

1. The client is written in C and I would like to unify the project's language
2. The host is currently hard to build using MinGW and is very Windows specific
3. The host is a jumbled mess of code from all the experimentation going on
4. I would eventually like to be able to port this to run on Linux guests

# Why C and not C++ (or some other language)

Beacuse I like C and for this project believe that C++ is overkill

# When will it be ready?

No idea

# Will it replace the C++ host?

Yes, but only when it is feature complete.

# Why doesn't this use CMake?

Because win-builds doesn't distribute it, so to make it easy for everyone to compile we do not require it.

# How do I build it?

Don't ask if you can't figure it out, this code is the very definition of experiemental and incomplete and should not be in use yet.

_-Geoff_
