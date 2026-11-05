Troubleshooting
###############

When diagnosing issues, start with the guide below. Avoid changing polling,
synchronization or renderer options until the basic server, shared-memory and
version checks pass.

.. toctree::
   :maxdepth: 1

   idd_diagnostics

.. _client_help_information:

Client information when asking for help
---------------------------------------

The Looking Glass client outputs a lot of useful information during normal
operation, which helps a lot with debugging and support when issues arise.

This information is available when running the client in a terminal.

When asking for help, it is important to include the following:

* version information about the client, and IDD;
* Linux distribution and kernel;
* X11 or Wayland and the compositor name;
* host and guest GPU models and drivers;
* whether ``lgmp:allowDMA`` is enabled;
* the guest resolution and refresh rate; and
* accurate reproduction steps that lead to your issue.

All of this information is helpful, and should not be excluded.

.. _issue_monitor_missing:

The Looking Glass monitor is missing in Windows
-----------------------------------------------

1. Confirm that the guest runs Windows 10 version 1903 or newer, or Windows 11.
2. Confirm that the IDD installer completed and restart Windows if requested.
3. Open the IDD helper and its log directory. See :ref:`idd_diagnostics`.
4. In ``looking-glass-idd.txt``, confirm that IVSHMEM opened with the expected
   size and that at least one configured display mode fits.
5. Increase the IVSHMEM size and restart the VM if every suitable mode was
   filtered.
   - See :ref:`libvirt_determining_memory`
6. Ensure the legacy host service is disabled. Only one Looking Glass Server
   implementation should use the IVSHMEM device.

A missing passed-through GPU does not by itself prevent the IDD display from
appearing. The IDD can start in software mode, although that mode is slower and
SDR-only.

.. _issue_stuck_on_loading:

The client remains on the waiting screen
----------------------------------------

* Confirm that the Windows Looking Glass monitor is active.
* Check that the client has selected the same KVMFR device or shared-memory file
  as the VM, if you have more than one.
* Confirm that the Linux user has read and write access to that device.
* Make sure you are using the same Looking Glass client version as the IDD.
* If ``/dev/kvmfr0`` is a regular file instead of a character device, stop the
  VM, remove that accidental file and load KVMFR before starting the VM again.
  See :ref:`KVMFR device verification <ivshmem_kvmfr_verification>`

.. _issue_image_slow:

The image is slow or stutters
-----------------------------

Open the timing graphs with :kbd:`ScrLk` + :kbd:`T` and follow
:ref:`client_performance`. Also check that:

* the IDD helper reports GPU acceleration rather than software processing;
* ``lgmp:allowDMA=yes`` is using a direct import when the host driver supports
  it;
* enough CPU cores remain available to Linux;
* the guest resolution and refresh do not exceed available memory bandwidth;
* the compositor is not adding an unexpected frame queue; and
* the fastest active client or OBS source is requesting the intended cadence.

Do not set the guest to an extreme refresh rate attempting to raise UPS. The IDD
can acquire newer frames and skip superseded ones before transport, but the
guest still pays the cost of rendering them.

.. _issue_keyboard_mouse_input:

Keyboard or mouse input does not work
-------------------------------------

* Press :kbd:`ScrLk` + :kbd:`I` to ensure guest input is enabled.
* Check ``looking-glass-input.txt`` and the client's selected input provider.
* Only one LGMP client owns direct input. Close another controlling client if
  necessary.
* If SPICE video fallback is active, keep ``spice:input=yes``. The client uses
  the VM's default PS/2 keyboard and mouse, and switches input to follow the
  video source.
* If ``input:captureOnly=yes``, press :kbd:`ScrLk` to enter capture mode.

See :ref:`client_input` for normal, automatic keyboard and capture modes.

.. _the_clipboard_is_not_working:

The clipboard is not working
----------------------------

-  **Is clipboard synchronization enabled?**

   -  Before you can copy or paste content between the guest and host,
      clipboard synchronization must be enabled through the ``clipboard:toVM``
      and ``clipboard:fromVM`` options. These default to on, but check that
      you haven't disabled them.

.. _keyboard_shortcuts_not_captured_on_gnome_wayland:

Keyboard shortcuts are not captured on GNOME Wayland
-----------------------------------------------------

Capture mode may fail to capture compositor shortcuts like
:kbd:`Alt` + :kbd:`Tab` or :kbd:`Alt` + :kbd:`Mouse3`—they go to GNOME instead of
the guest VM.

When Looking Glass first requests to inhibit shortcuts, GNOME shows a
dialog asking for permission. If you clicked "Deny" (or dismissed the dialog),
GNOME permanently blocks the application and never shows the dialog again.

Use the ``flatpak`` command to view or grant the permission, this works even
if LookingGlass is not a Flatpak application, as GNOME stores these permissions
in Flatpak's database:

.. code:: bash

   flatpak permission-set gnome shortcuts-inhibitor looking-glass-client.desktop GRANTED

To verify the permission was set:

.. code:: bash

   flatpak permissions gnome shortcuts-inhibitor

.. _issue_usb_audio_not_playing:

USB audio does not appear or play
---------------------------------

* Add an unused SPICE **USB Redirector** device to the VM.
* Keep ``spice:enable=yes``, ``spice:audio=yes`` and
  ``spice:usbAudio=yes``.
* In Windows Sound settings, select the Looking Glass USB Audio speakers as
  the output device.
* Check the client startup log. If USB audio creation failed before
  connection, Looking Glass falls back to classic SPICE audio and reports why.

.. seealso::

   :ref:`client_usb_audio`

.. _issue_microphone_missing:

The microphone is unavailable
------------------------------

The PipeWire audio backend supports recording from the microphone, but the
PulseAudio backend does not. Ensure the client has selected the PipeWire
backend, then check ``audio:micDefault`` and the microphone indicator. Press
:kbd:`ScrLk` + :kbd:`C` to change the default permission and :kbd:`ScrLk` +
:kbd:`E` to toggle recording.
