# Looking Glass

Looking Glass lets you use a Windows virtual machine from Linux with very low
display and input latency. The current recommended setup uses the Looking Glass
Indirect Display Driver (IDD) in the Windows guest and the Looking Glass Client
on the Linux host.

* Project Website: https://looking-glass.io
* Documentation: https://looking-glass.io/docs

## Start here

The Linux client is currently distributed as source code and must be built
before it can be installed. The end-user guide covers the complete process:

1. Build the Linux client.
2. Configure shared memory for the virtual machine.
3. Install the Looking Glass IDD in Windows.
4. Install and run the client.

See the [Looking Glass documentation](https://looking-glass.io/docs) for the
current requirements and setup guide.

## Source archives

❕❕❕ **IMPORTANT** ❕❕❕

This project contains submodules that must be checked out if building from the
git repository! If you are not a developer and just want to compile Looking
Glass, please download the source archive from the website instead:

https://looking-glass.io/downloads

Source code for the documentation can be found in the `doc` directory.

You may view this locally as HTML by running `make html` with `python3-sphinx`
and `python3-sphinx-rtd-theme` installed.
