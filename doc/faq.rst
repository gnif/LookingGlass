Frequently Asked Questions
##########################

General
-------

.. _how_does_looking_glass_work:

How does Looking Glass work?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Please see the following video that covers this:

https://www.youtube.com/watch?v=U44lihtNVVM

.. _can_i_feed_the_vm_directly_into_obs:

Can I feed the VM directly into OBS?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Looking Glass now features a functional :doc:`OBS plugin <obs>`, which acts as
another Looking Glass client, but instead gives the captured frames to OBS.

.. _why_is_my_ups_so_low:

Why is my UPS (Updates Per Second) so low?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

There are several reasons why this can happen, the most common are your
capture resolution, or refresh rate. The windows capture methods currently
struggle to capture high resolutions under certain circumstances.

Another cause can be how the game or application you are running is
configured. Because of the way windows integrate with the WDM (Windows
Desktop Manager), running applications in "Full Screen" mode may—in some
cases—cause a large performance penalty. Try switching to windowed
full-screen mode, the difference in performance can be like night and
day.

Some titles do some strange things at early initialization that cause
capture performance issues. One such title is the Unigine Valley
benchmark where the capture rate is limited to 1/2 the actual rate. For
an unknown reason to both myself and the Unigine developers a simple
task switch (alt+tab) in and out resolves the issue. This is not a
Looking Glass bug.

.. _is_my_gpu_supported:

Is my GPU supported?
~~~~~~~~~~~~~~~~~~~~

Your guest GPU almost certainly supports DXGI. Use DxDiag to confirm
that you have support for WDDM 1.2 or greater.

The server-side (guest) probing error "Capture is not possible,
unsupported device or driver" indicates NVidia duplication has failed,
not that DXGI has failed. You can fix the error by specifying
``-c DXGI``

.. _why_do_i_need_spice_if_i_dont_want_a_spice_display_device:

Why do I need Spice if I don't want a Spice display device?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

You don't need Display Spice enabled. Looking Glass has a Spice client
built in to provide some conveniences, but you can disable it with the
"-s" argument.

Note that without Spice, you will not be sending mouse/keyboard events
to the guest, nor will you get clipboard synchronization support.

.. _where_is_the_host_application_for_linux:

Where is the host application for Linux?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The "Windows host application" is actually the display server, which
runs in the guest VM. The only thing that needs to run in your Linux
host OS is the \`looking-glass-client\` application.

Mouse
-----

.. _the_mouse_is_jumpy_slow_laggy_when_using_spice:

The mouse is jumpy, slow, laggy when using SPICE
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Please be sure to install the SPICE guest tools from
https://www.spice-space.org/download.html#windows-binaries.

.. _how_to_enable_clipboard_synchronization_via_spice:

How to enable clipboard synchronization via SPICE
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Install the SPICE guest tools from
https://www.spice-space.org/download.html#windows-binaries.

Choose only one of the settings below (the one that applies to your
configuration):

-  QEMU

.. code:: bash

   -device virtio-serial-pci \
   -chardev spicevmc,id=vdagent,name=vdagent \
   -device virtserialport,chardev=vdagent,name=com.redhat.spice.0

-  libvirt

   -  Edit the VM using virsh ``sudo virsh edit VIRTUAL_MACHINE_NAME``
   -  Paste the code from below right above (note the closing tag)

.. code:: xml

     <!-- No need to add VirtIO Serial device, it will be added automatically -->
     <channel type="spicevmc">
       <target type="virtio" name="com.redhat.spice.0"/>
       <address type="virtio-serial" controller="0" bus="0" port="1"/>
     </channel>

.. _the_mouse_doesnt_stay_aligned_with_the_host.:

The mouse doesn't stay aligned with the host.
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

This is intentional. The host's mouse no longer interacts with your operating
system, and is completely captured by Looking Glass.

.. _the_cursor_position_doesnt_update_until_i_click:

The cursor position doesn't update until I click
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Make sure you have removed the Virtual Tablet Device from the Virtual
Machine. Due to the design of windows absolute pointing devices break
applications/games that require cursor capture and as such Looking Glass
does not support them.

Audio
-----

Looking Glass does not support audio routing. The preferred
solution is to pass through QEMU's audio to your host's audio system.

Another popular solution is to use
`Scream <https://github.com/duncanthrax/scream>`_, a virtual sound card which
pipes audio through the network. A guide for setting up scream is available on
the wiki: https://looking-glass.io/wiki/Using_Scream_over_LAN

.. _faq_win:

Windows
-------

.. _nvfbc_nvidia_capture_api_doesnt_work:

NvFBC (NVIDIA Capture API) doesn't work
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

NvFBC is only supported on professional-grade GPUs, and will not function on
consumer-grade cards like those from the GeForce series.

.. _the_screen_stops_updating_when_left_idle_for_a_time:

The screen stops updating when left idle for a time
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Windows is likely turning off the display to save power, you can prevent
this by adjusting the \`Power Options\` in the control panel.

.. _faq_host:

Host
----

Where is the log?
~~~~~~~~~~~~~~~~~

The log file for the host application is located at::

   %ProgramData%\Looking Glass (host)\looking-glass-host.txt

You can also find out where the file is by right clicking on the tray
icon and selecting "Log File Location".

The log file for the looking glass service is located at::

   %ProgramData%\Looking Glass (host)\looking-glass-host-service.txt

This is useful for troubleshooting errors related to the host
application not starting.

High priority capture using DXGI and Secure Desktop (UAC) capture support
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

By default Windows gives priority to the foreground application for any
GPU work which causes issues with capture if the foreground application
is consuming 100% of the available GPU resources. The looking glass host
application is able to increase the kernel GPU thread to realtime
priority which fixes this, but in order to do so it must run as the
``SYSTEM`` user account. To do this, Looking Glass needs to run as a
service. This can be accomplished by either using the NSIS installer
which will do this for you, or you can use the following command to
Install the service manually:

::

   looking-glass-host.exe InstallService

To remove the service use the following command:

::

   looking-glass-host.exe UninstallService

This will also enable the host application to capture the secure desktop
which includes things like the lock screen and UAC prompts.

.. _faq_host_admin_privs:

Why does the host require Administrator privileges?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

This is intentional for several reasons.

1. NvFBC requires a system wide hook to correctly obtain the cursor
   position as NVIDIA decided to not provide this as part of the cursor
   updates.
2. NvFBC requires administrator level access to enable the interface in
   the first place. (WIP)
3. DXGI performance can be improved if we have this. (WIP)

NvFBC (NVIDIA Frame Buffer Capture)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Why can't I compile NvFBC support into the host?
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

You must download and install the NVidia Capture SDK. Please note that
by doing so you will be agreeing to NVIDIA's SDK License agreement.

*-Geoff*

.. _a_note_about_ivshmem_and_scream_audio:

Why doesn't Looking Glass work with Scream over IVSHMEM?
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. warning::
   Using IVSHMEM with Scream may interfere with Looking Glass, as they may try
   to use the same device.

Please do not use the IVSHMEM plugin for Scream.
To fix this issue, use the default network transfer method.
The IVSHMEM method induces additional latency that is built into its
implementation. When using VirtIO for a network device the VM is already using
a highly optimized memory copy anyway so there is no need to make another one.

If you insist on using IVSHMEM for Scream—despite its inferiority to the
default network implementation—the Windows Host Application can be told
what device to use. Create a ``looking-glass-host.ini`` file in the same
directory as the looking-glass-host.exe file. In it, you can use the
``os:shmDevice`` option like so:

.. code:: INI

   [os]
   shmDevice=1

