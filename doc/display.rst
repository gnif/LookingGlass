.. _client_display:

Display and image quality
#########################

The EGL renderer is the supported client renderer and is selected
automatically. The older OpenGL renderer is deprecated, does not provide direct
DMA imports or native HDR, and should only be used to diagnose a compatibility
problem.

Wayland and X11 are both built by default. Automatic selection prefers Wayland
when ``WAYLAND_DISPLAY`` is set, then X11 when ``DISPLAY`` is set. To force X11
from a Wayland session for diagnosis, preserve ``DISPLAY`` and unset
``WAYLAND_DISPLAY`` for the client process.

.. _client_resolution:

Window and guest resolution
---------------------------

With the IDD, ``win:setGuestRes=yes`` asks Windows to match the client viewport
when its size changes. Press :kbd:`ScrLk` + :kbd:`=` to request the current
size immediately. The IDD creates this as its dynamic ExtraMode using the
default refresh configured in the IDD helper.

If a requested resolution does not fit in IVSHMEM, the IDD refuses it and the
helper reports the required size. See :ref:`libvirt_determining_memory`.

Useful window options include:

``win:autoResize``
   Resize the client window when the guest resolution changes.

``win:keepAspect``
   Preserve the guest aspect ratio while resizing.

``win:fullScreen``
   Start in borderless full-screen mode.

``win:rotate``
   Rotate the image by 0, 90, 180 or 270 degrees.

.. _client_scaling_filters:

Scaling and filters
-------------------

The overlay (enabled with :kbd:`ScrLk` + :kbd:`O`) has an **EGL filters** page,
which controls scaling, sharpening and custom processing. Filters run from top
to bottom and can be reordered. Save useful combinations as named presets, then
select one at startup with ``egl:preset``.

The built-in filters include:

* a configurable downscaler;
* AMD FidelityFX Super Resolution (FSR); and
* AMD FidelityFX Contrast Adaptive Sharpening (CAS).

The ``egl:scale`` option selects automatic, nearest-neighbor or linear
scaling when a filter does not provide the required scaling. Additional
mpv-style GLSL shaders may be loaded from ``eglFilter:glslPath``. The
`Anime4K <https://github.com/bloc97/Anime4K>`_ project provides compatible
GLSL filters.

.. _client_hdr:

HDR
---

The IDD exposes HDR when the Windows runtime provides the required IddCx
interfaces and the IDD is using a hardware render adapter. This normally means a
compatible Windows 11 guest. Windows 10 and IDD software processing still use
SDR.

Native HDR output on Linux requires all of the following:

* the EGL renderer;
* a Wayland compositor with ``color-management-v1`` support;
* an HDR-capable monitor and output configuration; and
* a host graphics driver that can present the required color format.

With ``egl:mapHDRtoSDR=yes`` the client tone-maps HDR frames for an SDR desktop.
Use ``egl:peakLuminance`` to describe the SDR display target and
``egl:maxCLL`` to limit the assumed content light level.

X11 presentation uses SDR, and the EGL renderer tone-maps HDR to SDR there by
default. Native Wayland PQ output also requires compositor support for ST 2084
and BT.2020, while scRGB requires compositor scRGB support.

.. _client_display_latency:

Presentation latency
--------------------

The Looking Glass client defaults favor low latency rather than conventional
buffered rendering. ``egl:vsync`` and ``egl:doubleBuffer`` can add latency and
should only be used when necessary.

``win:jitRender`` delays rendering toward the expected presentation deadline.
It can reduce the age of a frame at display time, but depends on stable timing.
Leave it disabled while diagnosing stalls or an unstable refresh cadence.
