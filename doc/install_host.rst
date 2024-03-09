.. _installing_host:

Host Application Installation
#############################

The Looking Glass Host application captures frames from the guest OS using a
capture API, and sends them to the
:ref:`client <client_install>`—be it on the host OS (hypervisor) or another
Virtual Machine—through a low-latency transfer protocol over shared memory.

You can get the host program in two ways:

-  Download a pre-built binary from https://looking-glass.io/downloads
   (**Recommended**)

-  Download the source code as described in :ref:`building`, then
   :ref:`build the host <host_building>`.

.. _host_install_linux:

For Linux
^^^^^^^^^

While the host application can be compiled and is somewhat functional for Linux
it is currently considered incomplete and not ready for usage. As such use at
your own risk and do not ask for support.

.. _host_install_osx:


For OSX
^^^^^^^

Currently there is no support or plans for support for OSX due to technical
limitations.

.. _host_install_windows:

For Windows
^^^^^^^^^^^

To begin, you must first run the Windows VM with the changes noted in the
:doc:`install_libvirt` section.

.. _installing_the_ivshmem_driver:

Installing the IVSHMEM driver
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Since B6 the host installer available on the official Looking Glass website
comes with the IVSHMEM driver and will install this for you. If you are running
an older version of Looking Glass please refer to the documentation for your
version.

.. _host_install_service:

Installing the Looking Glass service
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

After installing your IVSHMEM driver, we can now install the Looking Glass Host
onto our Windows Virtual Machine.

1. First, run ``looking-glass-host-setup.exe`` as an administrator
   (:ref:`Why? <faq_host_admin_privs>`)
2. You will be greeted by an intro screen. Press ``Next`` to continue.
3. You are presented with the |license| license. Please read and agree to the
   license by pressing ``Agree``.
4. You can change the install path if you wish, otherwise press ``Next`` to
   continue.
5. You may enable or disable options on this screen to configure the
   installation. The default values are recommended for most users.
   Press ``Install`` to begin installation.
6. After a few moments, installation will complete, and you will have a
   running instance of Looking Glass. If you experience failures, you can
   see them in the install log appearing in the middle of the window.
7. Press ``Close`` to exit the installer.

Command line users can run ``looking-glass-host-setup.exe /S`` to execute a
silent install with default options selected. Further configuration from the
command line can be done with flags. You can list all available flags by
running ``looking-glass-host-setup.exe /?``.
