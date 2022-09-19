Troubleshooting
###############

There are many different issues that can arise when setting up Looking
Glass. Below is a list of known issues with potential solutions:

.. _when_launching_looking_glass_the_desktop_doesnt_appear:

When launching Looking Glass the desktop doesn't appear
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

-  **Using an AMD GPU?**

   -  After the end of the Radeon HD Series, new AMD GPUs go to sleep when no
      display is connected. For this reason, one of two conditions must be met.

#. The GPU needs to remain plugged into a monitor (this is good for
   testing & troubleshooting).
#. The GPU needs to have a dummy plug installed which presents itself as a
   monitor.

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
