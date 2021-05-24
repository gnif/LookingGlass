Troubleshooting
###############

There are many different issues that can arise when setting up Looking
Glass. Below is a list of known issues with potential solutions:

.. _when_launching_looking_glass_the_desktop_doesnt_appear:

When Launching Looking Glass the Desktop Doesn't Appear
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

-  **Using an AMD GPU?**

   -  After the end of the Radeon HD Series AMD started incorporating a
      feature into their GPU's that effectively puts the card to sleep
      when no display is connected. For this reason one of two
      conditions need to be met.

#. The GPU needs to remain plugged into a monitor (this is good for
   testing & troubleshooting).
#. The GPU needs to have a plug like the ones used in GPU Crypto mining
   installed to trick the card into thinking that a display is
   connected.

.. _the_clipboard_is_not_working:

The Clipboard is not Working
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

-  **Did you enable it?**

   -  Before you can Copy/Paste content between the Guest and the Host
      you must `Enable Clipboard
      Synchronization <https://looking-glass.io/wiki/FAQ#How_to_enable_clipboard_synchronization_via_SPICE>`_

-  **Did you install the Spice Guest Tools?**

   -  Before you can Copy/Paste content between the Guest and the Host
      you must install the `SPICE Guest Tools
      driver <https://www.spice-space.org/download.html>`_. The
      download is labeled as "spice-guest-tools".

      .. warning::

         **NOTE: Do make sure that you do NOT install the QEMU Guest
         Tools driver. These are not the same.**

   -  **Did you install them twice?**

      -  The Spice VDAgent is available in both Spice Guest Tools, and
         standalone as a separate installer. Check your installed programs
         and uninstall the VDAgent if it's installed separately.

.. _followed_installation_instructions_but_looking_glass_still_doesnt_launch:

Followed Installation Instructions but Looking Glass Still Doesn't Launch
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Depending on your distribution, various circumstances can cause Looking
Glass to still not run. Below is a list of known issues that can prevent
Looking Glass from running properly.

AppArmor
^^^^^^^^

AppArmor is a security application that can prevent Looking Glass from
running. How to add security exceptions may vary on your distribution:

Start by opening the file ``/etc/apparmor.d/abstractions/libvirt-qemu``.
Now locate and edit the following lines to represent how they appear in
the example:

.. code:: text

   # for usb access
      /dev/bus/usb/** rw,
      /etc/udev/udev.conf r,
      /sys/bus/ r,
      /sys/class/ r,
      /run/udev/data/* rw,
      /dev/input/* rw,

   # Looking Glass
      /dev/shm/looking-glass rw,

Save, exit, and restart AppArmor:

.. code:: text

   sudo systemctl restart apparmor
