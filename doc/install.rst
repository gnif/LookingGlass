.. _installing:

Installation
############

.. _client_install:

Client
------

The Looking Glass Client receives frames from the :ref:`Host <host_install>` to
display on your screen. It also handles input, and can optionally share the
system clipboard with your guest OS through Spice.

First you must build the client from source, see :ref:`building`. Once you have
built the client, you can install it. Run the following as root::

   make install

To install for the local user only, run::

   cmake -DCMAKE_INSTALL_PREFIX=~/.local .. && make install

.. _client_libvirt_configuration:

libvirt Configuration
~~~~~~~~~~~~~~~~~~~~~

This article assumes you already have a fully functional libvirt domain with
PCI passthrough working on a dedicated monitor.

If you use virt-manager, this guide also applies to you, since virt-manager uses
libvirt as its back-end.

**If you are using QEMU directly, this does not apply to you.**

Add the following to your libvirt machine configuration inside the
'devices' section by running ``virsh edit <VM>`` where ``<VM>`` is the name of
your virtual machine.

.. code:: xml

   <shmem name='looking-glass'>
     <model type='ivshmem-plain'/>
     <size unit='M'>32</size>
   </shmem>

The memory size (show as 32 in the example above) may need to be
adjusted as per the :ref:`Determining Memory <client_determining_memory>` section.

.. _client_spice_server:

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
:ref:`client_clipboard_synchronization`

.. _client_apparmor:

AppArmor
^^^^^^^^

For libvirt versions before **5.10.0**, if you are using AppArmor, you
need to add permissions for QEMU to access the shared memory file. This
can be done by adding the following to
``/etc/apparmor.d/local/abstractions/libvirt-qemu``::

   /dev/shm/looking-glass rw,

then, restart AppArmor.

.. code:: bash

   sudo systemctl restart apparmor

.. _client_memballoon_tweak:

Memballoon
^^^^^^^^^^

The VirtIO memballoon device enables the host to dynamically reclaim memory
from your VM by growing the balloon inside the guest, reserving reclaimed
memory. Libvirt adds this device to guests by default.

However, this device causes major performance issues with VFIO passthrough
setups, and should be disabled.

Find the ``<memballoon>`` tag and set its type to ``none``:

.. code:: xml

   <memballoon model="none"/>

.. _client_qemu_commands:

QEMU Commands
~~~~~~~~~~~~~

**If you are using libvirt/virt-manager, then this does not apply to you.**

Add the following to the commands to your QEMU command line, adjusting
the ``bus`` parameter to suit your particular configuration:

.. code:: bash

   -device ivshmem-plain,memdev=ivshmem,bus=pcie.0 \
   -object memory-backend-file,id=ivshmem,share=on,mem-path=/dev/shm/looking-glass,size=32M

The memory size (shown as 32M in the example above) may need to be
adjusted as per :ref:`Determining Memory <client_determining_memory>` section.

.. _client_determining_memory:

Determining Memory
~~~~~~~~~~~~~~~~~~

You will need to adjust the memory size to be suitable for
your desired maximum resolution, with the following formula:

``width x height x 4 x 2 = total bytes``

``total bytes / 1024 / 1024 = total megabytes + 10``

For example, for a resolution of 1920x1080 (1080p):

``1920 x 1080 x 4 x 2 = 16,588,800 bytes``

``16,588,800 / 1024 / 1024 = 15.82 MB + 10 = 25.82 MB``

You must round this value up to the nearest power of two, which for the
provided example is 32MB.

.. _client_shmfile_permissions:

Shared Memory File Permissions
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The shared memory file used by IVSHMEM is found in ``/dev/shm/looking-glass``.
By default, it is owned by QEMU, and does not give read/write permissions to
your user, which are required for Looking Glass to run properly.

You can use `systemd-tmpfiles` to create the file before running your VM,
granting the necessary permissions which allow Looking Glass to use the file
properly.

Create a new file ``/etc/tmpfiles.d/10-looking-glass.conf``, and populate it
with the following::

   #Type Path               Mode UID  GID Age Argument

   f /dev/shm/looking-glass 0660 user kvm -

Change ``UID`` to the user name you will run Looking Glass with, usually your
own.

.. _client_clipboard_synchronization:

Clipboard Synchronization
~~~~~~~~~~~~~~~~~~~~~~~~~

Looking Glass can synchronize the clipboard between the host and guest using
the SPICE guest agent.

1. Install the SPICE guest tools from
https://www.spice-space.org/download.html#windows-binaries.

2. Configure your VM to enable the SPICE guest agent:

-  QEMU

.. code:: bash

   -device virtio-serial-pci \
   -chardev spicevmc,id=vdagent,name=vdagent \
   -device virtserialport,chardev=vdagent,name=com.redhat.spice.0

-  libvirt

.. code:: xml

     <channel type="spicevmc">
       <target type="virtio" name="com.redhat.spice.0"/>
       <address type="virtio-serial" controller="0" bus="0" port="1"/>
     </channel>
     <!-- No need to add a VirtIO Serial device, it will be added automatically -->

.. _host_install:

Host
----

The Looking Glass Host captures frames from the guest OS using a capture API,
and sends them to the :ref:`Client <client_install>`—be it on the host OS (hypervisor)
or another Virtual Machine—through a low-latency transfer protocol over shared
memory.

You can get the host program in two ways:

-  Download a pre-built binary from https://looking-glass.io/downloads
   (**recommended**)

-  Download the source code as described in :ref:`building`, then
   :ref:`build the host <host_building>`.

.. _host_install_windows:

Windows
~~~~~~~

To begin, you must first run the Windows VM with the changes noted above in
either the :ref:`client_libvirt_configuration` or :ref:`client_qemu_commands`
sections.

.. _installing_the_ivshmem_driver:

Installing the IVSHMEM Driver
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Windows will not prompt for a driver for the IVSHMEM device, instead, it
will use a default null (do nothing) driver for the device. To install
the IVSHMEM driver you will need to go into the device manager and
update the driver for the device "PCI standard RAM Controller" under the
"System Devices" node.

A signed Windows 10 driver can be obtained from Red Hat for this device
from the below address:

https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/upstream-virtio/

Please note that you must obtain version 0.1.161 or later.

If you encounter warnings or errors about driver signatures, ensure secure boot
is turned off in the bios/UEFI settings of your virtual machine.

.. _host_install_service:

Installing the Looking Glass Service
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

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
