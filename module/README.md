This kernel module implements a basic interface to the IVSHMEM device for
LookingGlass when using LookingGlass in VM->VM mode.

## Compiling (Manual)

Make sure you have your kernel headers installed first, on Debian/Ubuntu use
the following command.

    apt-get install linux-headers-$(uname -r)

Then simply run `make` and you're done.

### Loading

This module requires the `uio` module to be loaded first, loading it is as
simple as:

    modprobe uio
    insmod kvmfr.ko

## Compiling & Installing (DKMS)

You can install this module into DKMS so that it persists across kernel
upgrades. Simply run:

    dkms install .

### Loading 

Simply modprobe the module:

    modprobe kvmfr

## Usage

This will create the `/dev/kvmfr0` node that represents the KVMFR interface.
To use the interface you need permission to access it by either creating a
udev rule to ensure your user can read and write to it, or simply change it's
ownership manually, ie:

    sudo chown user:user /dev/kvmfr0

Usage with looking glass is simple, you only need to specify the path to the
device node, for example:

    ./looking-glass-client -f /dev/kvmfr0

