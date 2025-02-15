.. _requirements:

Requirements
############

.. _minimum:

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

.. _connected_display:

Connected Display
^^^^^^^^^^^^^^^^^

The GPU used for the guest virtual machine must have either a physical monitor
attached to it, or a cheap dummy plug. The guest operating system (most notably
Windows) will disable the GPU output if there is nothing attached to it and
Looking Glass will not be able to function. If you are using a vGPU the virtual
device should already have a virtual monitor attached to it negating this
requirement.

.. _recommended:

Recommended
-----------

At this time the recommended configuration is as follows:

* CPU 8 cores (16 threads) or better @ 3.0GHz or faster (full cores, not
  efficiency cores).

* Two discrete GPUs consisting of:

  * AMD or Intel brand GPU for the client application (usually your host system).
  * NVIDIA brand GPU for the guest system (virtual machine).

AMD or Intel for the client
^^^^^^^^^^^^^^^^^^^^^^^^^^^

AMD and Intel both support the `DMABUF` feature which enables offloading memory
transfers to the GPU hardware. Please note that making use of this feature
requires :doc:`loading the KVMFR kernel module <ivshmem_kvmfr>`.

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
Linux kernel unless you use the open source NVIDIA drivers.

.. _igpu_kvmfr_recommended:

iGPUs should use DMABUF
^^^^^^^^^^^^^^^^^^^^^^^

While `DMABUF` with the :doc:`KVMFR module <ivshmem_kvmfr>` offers performance
benefits for all users, for the often bandwidth-starved users with an iGPU on
their host it's considered a requirement for a decent experience.

When using a normal SHM file, many GPU drivers will copy incoming frames from
shared memory to an intermediary buffer, then upload it from that buffer to the
GPU's framebuffer. The KVMFR module will instead use Direct Memory Access (DMA)
to download incoming frames directly from shared memory, which may depending on
GPU design eliminate the intermediary buffer. This is especially helpful to iGPU
users as it frees up RAM bandwidth, which an iGPU already uses extensively.

An added benefit: since the upload is done with the iGPU, the CPU load is
reduced as the upload does not use processor cores.
