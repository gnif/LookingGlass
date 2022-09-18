.. _requrements:

Requirements
############

.. _minimum_:

Minimum
-------

The most basic requirement to make use of Looking Glass is to have a system
with two GPUs, the following configurations are valid:

* Two discrete GPUs (dGPU)
* A discrete GPU and an integrated (iGPU) such as is common in laptops.
* A discrete GPU or iGPU and a virtual GPU (vGPU) as supported by some
  hardware.

.. note::
  Please be aware that iGPU users may be limited in the resolution and refresh
  rate possible with Looking Glass due to the memory bandwidth limitations
  imposed due to the iGPU sharing system RAM for GPU usage.

Looking Glass aims to achieve the lowest possible latency and as such it
is important that you do not overload your system. The minimum recommended CPU
to obtain a decent experience with Looking Glass is 6 cores or more, with
Hyper-threading (>= 12 threads).

PCIe bandwidth can also be a limiting factor, as such both GPUs should have a
minimum of 8 lanes (x8) at PCIe3 speeds, or 4 lanes (x4) at PCIe4 speeds.

The GPU used for the guest virtual machine must have either a physical monitor
attached to it, or a cheap dummy plug. The guest operating system (most notably
Windows) will disable the GPU output if there is nothing attached to it and
Looking Glass will not be able to function. If you are using a vGPU the virtual
device should already have a virtual monitor attached to it negating this
requirement.

.. _recommended_:

Recommended
-----------

At this time the recommended configuration is as follows:

* CPU 8 cores (16 threads) or better @ 3.0GHz or faster (full cores, not
  efficiency cores).

* Two discrete GPUs consisting of:

  * AMD or Intel brand GPU for the client application (usually your host system).
  * NVIDIA brand GPU for the guest system (virtual machine).

The reason for these recommendations are as follows:

AMD or Intel for the client
^^^^^^^^^^^^^^^^^^^^^^^^^^^

AMD and Intel both support the `DMABUF` feature which enables offloading memory
transfers to the GPU hardware. Please note that making use of this feature
requires :doc:`loading the KVMFR kernel module <module>`.

Additionally AMD GPUs suffer stability issues when operating as a passthrough
device and as such we do not recommend their usage for such purposes. Models of
note that have issues include but are not limited to the entire Polaris, Vega,
Navi and BigNavi GPU series. Vega and Navi are notably the worst and should be
avoided for virtualization usage.

NVIDIA for the guest
^^^^^^^^^^^^^^^^^^^^

NVIDIA unlike AMD do not seem to suffer from the same stability issues as AMD
GPUs when operating as a passthrough GPU, however due to the closed source
nature of their drivers NVIDIA can not make use of the DMABUF feature in the
Linux kernel and as such it is not recommended for use as the host GPU.

`It has been said <https://github.com/NVIDIA/open-gpu-kernel-modules/discussions/243#discussioncomment-3283415>`_
that the open-source NVIDIA drivers as of release 525 will enable this support
and we have made changes to `an experiemental branch <https://github.com/gnif/LookingGlass/tree/dmabuf-test>`_
to support this, however until NVIDIA release this version and we can test these
changes against the driver, support will not be included in the release builds
of Looking Glass.
