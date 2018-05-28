This kernel module implements a basic interface to the IVSHMEM device for
LookingGlass when using LookingGlass in VM->VM mode.

## Compiling

Make sure you have your kernel headers installed first, on Debian/Ubuntu use
the following command.

    apt-get install linux-headers-$(uname -r)

Then simply run `make` and you're done.

## Usage

This module requires the `uio` module to be loaded first, loading it is as
simple as:

    modprobe uio
    insmod kvmfr.ko

This will create the `/dev/uio0` node that represents the KVMFR interface.
To use the interface you need permission to access it by either creating a
udev rule to ensure your user can read and write to it, or simply change it's
ownership manually, ie:

    sudo chown user:user /dev/uid0

Usage with looking glass is simple, but you do need to speecify both the path
and the size as LookingGlass can not determine the size by itself at this time.

    ./looking-glass-client -f /dev/uio0 -L 16

## Note

This module is not strictly required, it is possible to access the device
via the /sys interface directly, for example:

    ./looking-glass-client -f /sys/devices/pci0000:00/0000:00:03.0/resource2_wc

Obviously adjusting the PCI device IDs as required. However while this is
possible it is not recommended as access to the shared memory is much slower.
