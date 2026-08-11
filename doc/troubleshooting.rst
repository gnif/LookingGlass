Troubleshooting
###############

Start with the symptom below. Avoid changing polling, synchronization or
renderer options until the basic producer, shared-memory and version checks
pass.

.. toctree::
   :maxdepth: 1

   idd_diagnostics

.. _when_launching_looking_glass_the_desktop_doesnt_appear:

The Looking Glass monitor is missing in Windows
-----------------------------------------------

1. Confirm that the guest runs Windows 10 version 1803 or newer, or Windows 11.
2. Confirm that the IDD installer completed and restart Windows if requested.
3. Open the IDD helper and its log directory. See :ref:`idd_diagnostics`.
4. In ``looking-glass-idd.txt``, confirm that IVSHMEM opened with the expected
   size and that at least one configured display mode fits.
5. Increase IVSHMEM if every suitable mode was filtered. Restart the VM after
   changing its size.
6. Ensure the legacy Host service is disabled. Only one frame producer should
   use the Looking Glass IVSHMEM device.

A missing passed-through GPU does not by itself prevent the IDD display from
appearing. The IDD can start in software mode, although that mode is slower and
SDR-only.

The client remains on the waiting screen
----------------------------------------

* Confirm that the Windows Looking Glass monitor is active and producing a
  desktop.
* Check that the client selected the same KVMFR device or shared-memory file as
  the VM.
* Confirm that the Linux user has read and write access to that device.
* Use matching client and IDD releases. A KVMFR or LGMP protocol mismatch is a
  hard incompatibility.
* If ``/dev/kvmfr0`` is a regular file instead of a character device, stop the
  VM, remove that accidental file and load KVMFR before starting the VM again.

The image is slow or stutters
-----------------------------

Open the timing graphs with :kbd:`ScrLk` + :kbd:`T` and follow
:ref:`client_performance`. Also check:

* the IDD helper reports GPU acceleration rather than software processing;
* ``lgmp:allowDMA=yes`` is using a direct import when the host driver supports
  it;
* enough CPU cores remain available to Linux;
* the guest resolution and refresh do not exceed available memory bandwidth;
* the compositor is not adding an unexpected frame queue; and
* the fastest active client or OBS source is requesting the intended cadence.

Do not set the guest to an extreme refresh rate merely to raise UPS. The IDD
can acquire newer frames and skip superseded ones before transport, but the
guest still pays the cost of rendering them.

Keyboard or mouse input does not work
-------------------------------------

* Press :kbd:`ScrLk` + :kbd:`I` to ensure guest input is enabled.
* Check ``looking-glass-input.txt`` and the client's selected input provider.
* Only one LGMP client owns direct input. Close another controlling client if
  necessary.
* If SPICE video fallback is active, keep ``spice:input=yes``. The client uses
  the VM's default PS/2 keyboard and mouse and intentionally switches input
  with the video source.
* If ``input:captureOnly=yes``, press :kbd:`ScrLk` to enter capture mode.

See :ref:`client_input` for normal, automatic keyboard and capture modes.

.. _the_clipboard_is_not_working:

The clipboard is not working
----------------------------

-  **Is clipboard synchronization enabled?**

   -  Before you can copy or paste content between the guest and host,
      :ref:`clipboard
      synchronization <libvirt_clipboard_synchronization>`
      must be enabled.

-  **Did you install the Spice Guest Tools?**

   -  The `SPICE Guest Tools
      driver <https://www.spice-space.org/download.html>`_ must be installed
      inside the Windows guest to synchronize the clipboard.
      The download is labeled "spice-guest-tools".

      .. warning::

         Do **NOT** install the QEMU Guest Tools driver.
         These are not the same.

   -  **Is it installed twice?**

      -  The Spice VDAgent is available in both Spice Guest Tools, and
         standalone as a separate installer. Check your installed programs
         and uninstall the VDAgent if it's installed separately.

.. _keyboard_shortcuts_not_captured_on_gnome_wayland:

Keyboard shortcuts are not captured on GNOME Wayland
-----------------------------------------------------

Capture mode may fail to capture compositor shortcuts like
ALT+Tab or ALT+Middle Mouse - they go to GNOME instead of the guest VM.

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

USB audio does not appear or play
---------------------------------

* Confirm that the client was built with ``libusbredirparser-0.5`` version
  0.7.1 or newer.
* Add an unused SPICE **USB Redirector** device to the VM.
* Keep ``spice:enable=yes``, ``spice:audio=yes`` and
  ``spice:usbAudio=yes``.
* In Windows Sound settings, select the Looking Glass USB Audio speakers as
  the output device.
* Check the client startup log. If USB audio creation failed before
  connection, Looking Glass falls back to classic SPICE audio and reports why.

The microphone is unavailable
------------------------------

The current PipeWire backend supports recording; the PulseAudio backend does
not. Ensure the client selected PipeWire, then check ``audio:micDefault`` and
the microphone indicator. Press :kbd:`ScrLk` + :kbd:`C` to change the default
permission and :kbd:`ScrLk` + :kbd:`E` to toggle recording.
