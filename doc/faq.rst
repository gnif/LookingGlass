Frequently asked questions
##########################

General
-------

.. _how_does_looking_glass_work:

How does Looking Glass work?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The Windows IDD creates a virtual monitor and places completed frames in an
IVSHMEM region shared with Linux. The client imports the newest frame, renders
it and returns input through the available transport. The image is not encoded
as a video stream, which avoids codec latency and quality loss.

See :doc:`overview` for the current components. A detailed video explanation
is also available at https://www.youtube.com/watch?v=U44lihtNVVM.

.. _can_i_feed_the_vm_directly_into_obs:

Can I feed the VM directly into OBS?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Yes. The :doc:`OBS plugin <obs>` is an independent frame consumer and does not
capture the client window.

.. _why_is_my_ups_so_low:

Why is UPS lower than the guest refresh rate?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

UPS counts frames delivered to that consumer, not every frame Windows may
render. The IDD publishes to the deadline requested by the fastest active
client and can skip guest frames that have already been superseded. This saves
memory bandwidth without deliberately adding a frame of latency.

If UPS is below the active consumer's requested rate, use
:ref:`client_performance` to locate the slow stage. High resolution, software
processing, memory bandwidth, host scheduling and compositor presentation can
all impose a lower limit.

.. _is_my_gpu_supported:

Is a passed-through GPU required?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

No. The IDD can create an SDR display using software processing when it cannot
use a Windows render adapter. A hardware adapter is strongly recommended for
lower latency, higher refresh rates, HDR, and cadence scheduling.

The Linux client still needs an EGL-capable host graphics driver. Direct
DMA-BUF import depends on the host GPU and driver; the client falls back to a
copy when it is unavailable.

.. _why_do_i_need_spice_if_i_dont_want_a_spice_display_device:

Do I need SPICE?
~~~~~~~~~~~~~~~~

SPICE is not required for the primary IDD video or direct input paths. It is
still useful for clipboard, audio, input fallback and automatic video fallback.
These services are selected independently.

Set ``spice:enable=no`` or use ``-s`` to disable SPICE completely. Disable an
individual service with ``spice:input``, ``spice:clipboard`` or
``spice:audio`` instead when the other services are still wanted.

.. _where_is_the_host_application_for_linux:

Which application runs on Linux?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The Looking Glass Client runs on the Linux host operating system. The IDD runs
inside the Windows guest. The older product named the **Host Application** is a
legacy Windows server; “Host” in that name does not mean the Linux host OS.

Input and window system
-----------------------

.. _gnome_wayland_decorations:

Why is there no title bar on GNOME Wayland?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

GNOME does not implement the standard Wayland server-side decoration
protocol. Build Looking Glass with libdecor support:

.. code:: bash

   cmake -DENABLE_LIBDECOR=ON ../

Install ``libdecor-0-dev`` first on Debian-based systems. Alternatively, hold
the Super key and right-click the window to use the compositor's move and
resize menu.

.. _the_mouse_is_jumpy_slow_laggy_when_using_spice:

Why is SPICE fallback mouse movement different?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The IDD's direct input path uses absolute positioning for normal desktop use
and relative movement in capture mode. SPICE input is relative-only, so guest
mouse acceleration can change its feel or cause temporary position error.

Use the direct IDD input path when available. For a game, use capture mode and
consider ``input:rawMouse=yes``.

.. _the_cursor_position_doesnt_update_until_i_click:

Should I add a virtual tablet?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

No. Looking Glass does not require an additional virtio tablet, mouse or
keyboard. Direct IDD input provides its own absolute and relative devices, and
SPICE fallback uses the VM's default PS/2 input devices.

Audio
-----

Does Looking Glass support microphone input and surround sound?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Yes. The recommended emulated USB Audio 2.0 device provides stereo,
quadraphonic, 5.1 and 7.1 playback plus stereo recording at rates up to
192 kHz. Classic SPICE audio also provides playback and recording. Client
microphone recording currently requires the PipeWire backend. See
:doc:`audio`.

Legacy Host Application
-----------------------

The sections below apply only to the legacy Windows Host Application. New
installations should use the IDD. See :ref:`legacy_host_policy`.

.. _faq_host:

Where is the legacy Host log?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The application log is:

``%ProgramData%\Looking Glass (host)\looking-glass-host.txt``

The service log is:

``%ProgramData%\Looking Glass (host)\looking-glass-host-service.txt``

.. _faq_host_admin_privs:

Why does the legacy Host require administrator privileges?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The legacy capture APIs use privileged functions for GPU scheduling, NvFBC
setup and secure-desktop capture. The installer runs it as a Windows service
under the SYSTEM account. This is not how the current IDD is configured.

.. _nvfbc_nvidia_capture_api_doesnt_work:

Why does NvFBC not work?
~~~~~~~~~~~~~~~~~~~~~~~~

NvFBC is a legacy Host capture method and requires supported NVIDIA hardware
and SDK licensing. It is not part of the IDD path.

.. _the_screen_stops_updating_when_left_idle_for_a_time:

Why does a legacy captured display stop when idle?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Windows may turn off the physical or dummy display captured by the legacy
Host. Disable display sleep for that monitor. The IDD virtual-monitor path does
not require a physical display.

.. _a_note_about_ivshmem_and_scream_audio:

Can Scream and Looking Glass share IVSHMEM?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Do not use Scream with Looking Glass. Use the built-in classic SPICE or USB
audio path instead. In particular, Scream's IVSHMEM transport can select or
interfere with the shared device used for Looking Glass frames.

Technical details
-----------------

.. toctree::
   :maxdepth: 1

   tech_faq
