Installation
############

.. _looking_glass_client:

Looking Glass Client
--------------------

This guide will step you through building the looking glass client from
source, before you attempt to do this you should have a basic
understanding of how to use the shell.

.. _building_the_application:

Building the Application
~~~~~~~~~~~~~~~~~~~~~~~~

.. _installing_build_dependencies:

Installing Build Dependencies
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

These required libraries and tools should be installed first.

.. _required_dependencies:

Required Dependencies
'''''''''''''''''''''

-  cmake
-  gcc \| clang
-  fonts-freefont-ttf
-  libegl-dev
-  libgl-dev
-  libfontconfig1-dev
-  libgmp-dev
-  libsdl2-dev
-  libsdl2-ttf-dev
-  libspice-protocol-dev
-  make
-  nettle-dev
-  pkg-config

.. _may_be_disabled:

May be disabled
<<<<<<<<<<<<<<<

These dependencies are required by default, but may be omitted if their
feature is disabled when running :ref:`cmake <client_building>`.

-  Disable with ``cmake -DENABLE_BACKTRACE=no``

   -  binutils-dev

-  Disable with ``cmake -DENABLE_X11=no``

   -  libx11-dev
   -  libxfixes-dev
   -  libxi-dev
   -  libxss-dev

-  Disable with ``cmake -DENABLE_WAYLAND=no``

   -  libwayland-bin
   -  libwayland-dev
   -  wayland-protocols

You can fetch these dependencies on Debian systems with the following command:

``apt-get install binutils-dev cmake fonts-freefont-ttf libfontconfig1-dev
libsdl2-dev libsdl2-ttf-dev libspice-protocol-dev libx11-dev nettle-dev
wayland-protocols``

Downloading
^^^^^^^^^^^

Either visit the site at `Looking Glass Download
Page <https://looking-glass.io/downloads>`_

Or pull the lastest **bleeding-edge version** using the **git** command.

.. note::

   If you are using the latest bleeding-edge from the master branch
   you MUST download/use the corresponding host application

.. code:: bash

   git clone --recursive https://github.com/gnif/LookingGlass.git

.. _client_building:

Building
^^^^^^^^

If you downloaded the file via the web link then you should have a 'zip'
file. Simply unzip and cd into the new directory. If you used 'git' then
cd into the 'LookingGlass' directory.

.. code:: bash

   mkdir client/build
   cd client/build
   cmake ../
   make

.. note::

   The most common compile error is related to backtrace support. This can be
   disabled by adding the following option to the cmake command:
   **-DENABLE_BACKTRACE=0**, however, if you disable this and need support for a
   crash please be sure to use gdb to obtain a backtrace manually or there is
   nothing that can be done to help you.

Should this all go well you should be left with the file
**looking-glass-client**. Before you run the client you will first need
to configure either Libvirt or Qemu (whichever you prefer) and then set
up the Windows side service.

You can call the client from the build directory; or, you can make it
callable generally by adding the directory to your path or issuing

.. code:: bash

   ln -s $(pwd)/looking-glass-client /usr/local/bin/

from the build directory.

.. _libvirt_configuration:

libvirt Configuration
~~~~~~~~~~~~~~~~~~~~~

This article assumes you already have a fully functional libvirt VM with
PCI Passthrough working on a dedicated monitor. If you do not please
ensure this is configured before you proceed.

If you use virt-manager, this guide also applies to you, since it uses
libvirt.

**If you are using QEMU directly, this does not apply to you.**

Add the following to the libvirt machine configuration inside the
'devices' section by running "virsh edit VM" where VM is the name of
your virtual machine.

.. code:: xml

   <shmem name='looking-glass'>
     <model type='ivshmem-plain'/>
     <size unit='M'>32</size>
   </shmem>

The memory size (show as 32 in the example above) may need to be
adjusted as per the :ref:`Determining Memory <determining_memory>` section.

.. _spice_server:

Spice Server
^^^^^^^^^^^^

If you would like to use Spice to give you keyboard and mouse input
along with clipboard sync support, make sure you have a
``<graphics type='spice'>`` device, then:

-  Find your ``<video>`` device, and set ``<model type='none'/>``

   -  If you can't find it, make sure you have a ``<graphics>``
      device, save and edit again
   -  On older libvirt versions, just disable the device in Windows
      Device Manager

-  Remove the ``<input type='tablet'/>`` device, if you have one
-  Create an ``<input type='mouse'/>`` device, if you don't already have one
-  Create an ``<input type='keyboard' bus='virtio'/>`` device to improve
   keyboard usage

   -  This requires the *vioinput* driver from
      `virtio-win <https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/stable-virtio/>`_
      to be installed in the guest

If you want clipboard synchronization please see
:ref:`how_to_enable_clipboard_synchronization_via_spice`

AppArmor
^^^^^^^^

For libvirt versions before **5.10.0**, if you are using AppArmor, you
need to add permissions for QEMU to access the shared memory file. This
can be done by adding the following to
*/etc/apparmor.d/abstractions/libvirt-qemu*.

``/dev/shm/looking-glass rw,``

.. _qemu_commands:

Qemu Commands
~~~~~~~~~~~~~

**If you are using virt manager/libvirt then this does not apply to
you.**

Add the following to the commands to your QEMU command line, adjusting
the bus to suit your particular configuration:

.. code:: bash

   -device ivshmem-plain,memdev=ivshmem,bus=pcie.0 \
   -object memory-backend-file,id=ivshmem,share=on,mem-path=/dev/shm/looking-glass,size=32M

The memory size (show as 32 in the example above) may need to be
adjusted as per :ref:`Determining Memory <determining_memory>` section.

.. _determining_memory:

Determining Memory
~~~~~~~~~~~~~~~~~~

You will need to adjust the memory size to a value that is suitable for
your desired maximum resolution using the following formula:

``width x height x 4 x 2 = total bytes``

``total bytes / 1024 / 1024 = total megabytes + 2``

For example, for a resolution of 1920x1080 (1080p)

``1920 x 1080 x 4 x 2 = 16,588,800 bytes``

``16,588,800 / 1024 / 1024 = 15.82 MB + 2 = 17.82``

You must round this value up to the nearest power of two, which with the
above example would be 32MB

Note: This formula may be out of date. A 1440p display requires 64mb
shared memory.

The shared memory file will be located in /dev/shm/looking-glass and
will need to be created on every boot otherwise it will have incorrect
permissions. Looking glass will not be able to run unless it has
permissions to this file. You can create the shared memory file
automatically by adding the following config file:

``touch /etc/tmpfiles.d/10-looking-glass.conf``

Add the following content to the file::

   #Type Path Mode UID GID Age Argument

   f /dev/shm/looking-glass 0660 user kvm -

Be sure to set the UID to your local user.

.. _looking_glass_service_windows:

Looking Glass Service (Windows)
-------------------------------

You must first run the Windows VM with the changes noted above in either
the :ref:`libvirt_configuration` or :ref:`qemu_commands` sections.

.. _installing_the_ivshmem_driver:

Installing the IVSHMEM Driver
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Windows will not prompt for a driver for the IVSHMEM device, instead, it
will use a default null (do nothing) driver for the device. To install
the IVSHMEM driver you will need to go into the device manager and
update the driver for the device "PCI standard RAM Controller" under the
"System Devices" node.

A signed Windows 10 driver can be obtained from Red Hat for this device
from the below address:

https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/upstream-virtio/

Please note that you must obtain version 0.1.161 or later.

If the installation of the driver results in warnings or errors about
driver signatures, ensure secure boot is turned off for the virtual
machine bios/uefi.

.. _a_note_about_ivshmem_and_scream_audio:

A note about IVSHMEM and Scream Audio
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. warning::
   Using IVSHMEM with Scream may interfere with Looking Glass, as they may try
   to use the same device.

Please do not use the IVSHMEM plugin for Scream.
Use the default network transfer method. The IVSHMEM method induces
additional latency that is built into its implementation. When using
VirtIO for a network device the VM is already using a highly optimized
memory copy anyway so there is no need to make another one.

If you insist on using IVSHMEM for Scream—despite its inferiority to the
default network implementation—the Windows Host Application can be told
what device to use. Create a ``looking-glass-host.ini`` file in the same
directory as the looking-glass-host.exe file. In it, you can use the
``os:shmDevice`` option like so:

.. code:: INI

   [os]
   shmDevice=1

.. _using_the_windows_host_application:

Using the Windows Host Application
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Start by downloading the correct version for your release from
https://looking-glass.io/downloads. You can either choose between
**Official Releases**, which are stable; or **Release Candidates**, new versions
about to be stable, but haven't passed validation.

.. note::
   If your **looking-glass-client** was created by building from the **master
   branch** you have to pick the **Bleeding Edge** version.

Next, extract the zip archive using the commit hash for the password.
Then, run the ``looking-glass-host-setup.exe`` installer
and click through it. By default, the installer will install a service that
automatically starts the host application at boot. The installer can
also be installed in silent mode with the ``/S`` switch. Other command
line options for the installer are documented by running it with the
``/h`` switch. There is also an unofficial Chocolatey package available,
install with ``choco install looking-glass-host --pre``.

The windows host application captures the windows desktop and stuffs the
frames into the shared memory via the shared memory virtual device,
without this Looking Glass will not function. It is critical that the
version of the host application matches the version of the client
application, as differing versions can be, and usually are,
incompatible.

.. note::
   As of 2020-10-23, Microsoft Defender is known to mark the
   Looking-Glass host executable as a virus and in some cases will
   automatically delete the file.

.. _running_the_client:

Running the Client
------------------

The client command is the binary file: **looking-glass-client**. This
command should run after the Windows Host Application has started.

For an updated list of arguments visit:
https://github.com/gnif/LookingGlass/blob/master/client/README.md

Common options include ``-s`` for disabling spice, ``-S`` for disabling the
screen saver, and ``-F`` to automatically enter full screen.
