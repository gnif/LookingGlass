.. _requirements:

Requirements and compatibility
##############################

.. _minimum:

Required
--------

Linux host
   A Linux system capable of running KVM/QEMU and building the Looking Glass
   Client. The client requires an EGL-capable graphics driver. X11 and Wayland
   are supported. For a responsive passthrough setup, use at least six CPU
   cores with twelve hardware threads.

Windows guest
   A Windows 10 version 1803 or newer, or Windows 11, virtual machine. The
   recommended IDD path can create its own virtual monitor and does not require
   a physical monitor or dummy plug.

Shared memory
   An IVSHMEM device large enough for the maximum guest resolution. The KVMFR
   kernel module is recommended and is required for direct GPU imports.

Matching components
   Use the current client, IDD and OBS plugin from the same Looking Glass
   release. A protocol version mismatch is not supported. Legacy Host users
   must use the complete matching B7 stack described below.

The IDD can fall back to software processing if the Windows guest has no
suitable render GPU. This is useful for compatibility, but hardware processing
is strongly recommended for lower latency and higher frame rates. Software
mode is SDR-only and does not provide the same performance as the GPU path.

.. _recommended:

Recommended
-----------

For a responsive high-resolution setup, use:

* a host CPU with eight cores and sixteen threads or better, with full
  performance cores around 3 GHz or faster;
* KVMFR rather than a plain shared-memory file;
* a host GPU and driver that support direct DMA imports;
* a hardware render adapter in the Windows guest; and
* enough memory bandwidth for the chosen resolution and refresh rate.

Do not assign every CPU core to the guest. Reserve at least two physical CPU
cores, or four threads, for Linux. Looking Glass, QEMU, the Linux desktop and
audio server all need host CPU time. High refresh rates also raise
shared-memory and GPU bandwidth requirements.

For a passed-through GPU, PCIe bandwidth can also limit performance. Use at
least eight PCIe 3.0 lanes or four PCIe 4.0 lanes where practical.

.. _connected_display:

Physical display and dummy plugs
--------------------------------

The recommended IDD does not require a physical display or dummy plug. It
creates a virtual Windows monitor, including on systems without a
passed-through display output.

A physical display, dummy plug or another virtual monitor is only required
when using the :doc:`legacy Host Application <install_host>`, because that
application captures an existing Windows display.

.. _igpu_kvmfr_recommended:

Host GPU notes
--------------

AMD and Intel host GPUs commonly support the DMA-BUF path used by KVMFR. This
can reduce CPU work and avoid an extra system-memory copy. It is particularly
important for integrated GPUs, which already share memory bandwidth with the
CPU.

NVIDIA host GPUs require a driver configuration that supports DMA-BUF import.
If direct import is unavailable, the client falls back to a software copy.
Looking Glass still works, but uses more CPU and memory bandwidth.

Windows and HDR
---------------

Windows 10 remains supported. Current HDR and wide-color-gamut display features
depend on newer IddCx interfaces that the driver checks at runtime, so they are
normally available only on a compatible Windows 11 installation. The IDD
continues to use its SDR-compatible path when those interfaces are unavailable.

Native HDR presentation on Linux requires the EGL renderer, Wayland
``color-management-v1`` support in the compositor, and an HDR-capable output.
When native HDR is unavailable, the client can map HDR content to SDR.

Legacy Host compatibility
-------------------------

The Windows Host Application is no longer the recommended producer. B7 is the
last recommended version for users who require its non-capture mouse input
workflow. Use the complete matching B7 stack for that workflow rather than
mixing B7 and current components.
