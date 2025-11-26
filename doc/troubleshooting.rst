Troubleshooting
###############

There are many different issues that can arise when setting up Looking
Glass. Below is a list of known issues with potential solutions:

.. _when_launching_looking_glass_the_desktop_doesnt_appear:

When launching Looking Glass the desktop doesn't appear
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Make sure you meet the :ref:`minimum requirements<minimum>` for using
Looking Glass, especially regarding your guest GPU. See
:ref:`connected_display` for more details.

.. _the_clipboard_is_not_working:

The clipboard is not working
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

-  **Is clipboard synchronization enabled?**

   -  Before you can copy or paste content between the guest and host,
      :ref:`clipboard
      synchronization <libvirt_clipboard_synchronization>`
      must be enabled.

-  **Did you install the Spice Guest Tools?**

   -  The `SPICE Guest Tools
      driver <https://www.spice-space.org/download.html>`_ must be installed
      on the host OS to synchronize the clipboard.
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
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

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

