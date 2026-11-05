.. _installing_idd:

Looking Glass IDD
#################

The Looking Glass IDD is the recommended Looking Glass server implementation for
Windows guests. Download the ``looking-glass-idd-setup.exe`` installer that
matches the client release, run it as an administrator inside the Windows guest,
and follow the installer.

The installer includes the display driver, direct input driver and IDD helper.
It can also install the IVSHMEM driver. If the legacy host service is already
installed, allow the installer to disable it so that only one Looking Glass
server is active.

The display may briefly disappear while Windows installs or reloads the
driver. Restart Windows if the installer requests it.

.. _install_idd:

Install
-------

1. `Download the IDD installer <https://looking-glass.io/downloads>`__ from the
   same release as the client.
2. Run ``looking-glass-idd-setup.exe`` as an administrator in the guest.
3. Select **IVSHMEM Driver** if a suitable IVSHMEM driver is not already
   installed.
4. Leave **Indirect Display Driver (IDD)** selected.
5. If offered, leave **Disable old host app** selected. Running two servers
   against the same shared-memory device is not supported.
6. Complete the installation and restart Windows if requested.

After installation, Windows should show a display named Looking Glass and the
IDD helper should appear in the notification area. The helper icon reports
whether the IDD is using GPU acceleration or software processing.

The IDD installer also installs its direct input driver, there is no separate
input package to install.

.. _upgrade_idd:

Upgrade
-------

1. Download the desired version of the IDD installer and run it over the
   existing installation.
2. Close all active clients and OBS sources.
3. Upgrade the Looking Glass client and OBS plugin to the same version as the
   IDD.

Windows may briefly remove and recreate the virtual display. Restart Windows
when the installer reports that a restart is required.

.. _uninstall_idd:

Uninstall
---------

 - Remove **Looking Glass (IDD)** from Windows' **Installed apps** or **Programs
   and Features**.

This removes both the display and input drivers and deletes
custom IDD modes and preferences. The optional IVSHMEM files are removed from
the Looking Glass installation directory, but an IVSHMEM driver package that
was already installed in Windows is not uninstalled.

If you are installing the legacy host application, re-enable or reinstall the
host service only after the IDD has been removed.

.. _silent_install_idd:

Silent installation
-------------------

The installer accepts these (case-sensitive) options for managed installations:

``/S``
   Install silently.

``/ivshmem``
   Install the bundled IVSHMEM driver when it is present in the installer.

``/D=path``
   Change the installation directory. This must be the final option and must
   not be quoted.

.. _configure_idd:

Configuration and diagnostics
-----------------------------

.. toctree::
   :maxdepth: 1

   idd_configuration

For faults, the helper can open the IDD log directory directly. See
:ref:`idd_diagnostics` for the files to collect.
