.. _installing_idd:

Looking Glass IDD
#################

.. _install_idd:

The Looking Glass IDD is the recommended Looking Glass server implementation for
Windows guests. Download the ``looking-glass-idd-setup.exe`` installer that
matches the client release, run it as an administrator inside the Windows guest
and follow the installer.

The installer includes the display driver, direct input driver and IDD helper.
It can also install the IVSHMEM driver. If the legacy Host service is present,
allow the installer to disable it so that only one Looking Glass server is
active.

The display may briefly disappear while Windows installs or reloads the
driver. Restart Windows if the installer requests it.

Install
-------

1. Download the IDD installer from the same release as the client source.
2. Run ``looking-glass-idd-setup.exe`` as an administrator in Windows.
3. Select **IVSHMEM Driver** if a suitable IVSHMEM driver is not already
   installed in the guest.
4. Leave **Indirect Display Driver (IDD)** selected.
5. If offered, leave **Disable old host app** selected. Running two servers
   against the same shared-memory device is not supported.
6. Complete the installation and restart Windows if requested.

After installation, Windows should show a display named Looking Glass and the
IDD helper should appear in the notification area. The helper icon reports
whether the IDD is using GPU acceleration or software processing.

The IDD installer also installs its direct input driver. There is no separate
input package to install.

Upgrade
-------

Close active clients and OBS sources, then run the new matching IDD installer
over the existing installation. Upgrade the Linux client and OBS plugin to the
same release before using them again.

Windows may briefly remove and recreate the virtual display. Restart Windows
when the installer reports that a restart is required.

Uninstall
---------

Remove **Looking Glass (IDD)** from Windows **Installed apps** or **Programs
and Features**. This removes both the display and input drivers and deletes
custom IDD modes and preferences. The optional IVSHMEM files are removed from
the Looking Glass installation directory, but an IVSHMEM driver package that
was already installed in Windows is not uninstalled.

If you are returning to the legacy Host Application, re-enable or reinstall
its service only after the IDD has been removed.

Silent installation
-------------------

The installer accepts these options for managed installations:

``/S``
   Install silently. The uppercase spelling is required.

``/ivshmem``
   Install the bundled IVSHMEM driver when it is present in the installer.

``/D=path``
   Change the installation directory. This must be the final option and must
   not be quoted.

Configuration and diagnostics
-----------------------------

.. toctree::
   :maxdepth: 1

   idd_configuration

For faults, the helper can open the IDD log directory directly. See
:ref:`idd_diagnostics` for the files to collect.
