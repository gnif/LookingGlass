.. _installing:

Installation
############

.. _libvirt:

libvirt/QEMU configuration
--------------------------

This article assumes you already have a fully functional libvirt domain with
PCI passthrough working.

If you use virt-manager, this guide also applies to you, since virt-manager uses
libvirt as its back-end.

.. _libvirt_ivshmem:

IVSHMEM
^^^^^^^

Configuration
~~~~~~~~~~~~~

.. note::
  If your host GPU is either AMD or Intel it is better to set this up using the
  KVMFR kernel module as this will allow you to make use of DMA transfers to
  offload some of the memory transfers to the GPU.
  See `VM->host` in :ref:`kernel_module`.

Add the following to your libvirt machine configuration inside the
'devices' section by running ``virsh edit <VM>`` where ``<VM>`` is the name of
your virtual machine.

.. code:: xml

   <shmem name='looking-glass'>
     <model type='ivshmem-plain'/>
     <size unit='M'>32</size>
   </shmem>

.. note::
  If you are using QEMU directly without libvirt the following arguments are
  required instead.

  Add the following to the commands to your QEMU command line, adjusting
  the ``bus`` parameter to suit your particular configuration:

  .. code:: bash

     -device ivshmem-plain,memdev=ivshmem,bus=pcie.0 \
     -object memory-backend-file,id=ivshmem,share=on,mem-path=/dev/shm/looking-glass,size=32M

The memory size (show as 32 in the example above) may need to be
adjusted as per the :ref:`Determining memory <libvirt_determining_memory>`
section.

.. warning::
  If you change the size of this after starting your virtual machine you may
  need to remove the file `/dev/shm/looking-glass` to allow QEMU to re-create
  it with the correct size. If you do this the permissions of the file may be
  incorrect for your user to be able to access it and you will need to correct
  this. See :ref:`libvirt_shmfile_permissions`

.. _libvirt_determining_memory:

Determining memory
~~~~~~~~~~~~~~~~~~

You will need to adjust the memory size to be suitable for your desired maximum
resolution, with the following formula:

.. code:: text
   
  width x height x pixel size x 2 = frame bytes

  frame bytes / 1024 / 1024 = frame megabytes

  frame megabytes + 10 MiB = total megabytes

Where `pixel size` is 4 for 32-bit RGB (SDR) or 8 for 64-bit
(HDR :ref:`* <libvirt_determining_memory_hdr>`).

Failure to do so will cause Looking Glass to truncate the bottom of the screen
and will trigger a message popup to inform you of the size you need to increase
the value to.

For example, for a resolution of 1920x1080 (1080p):

.. code:: text

  1920 x 1080 x 4 x 2 = 16,588,800 bytes

  16,588,800 / 1024 / 1024 = 15.82 MiB

  15.82 MiB + 10 MiB = 25.82 MiB

You must round this value up to the nearest power of two, which for the
provided example is 32 MiB.

.. note::
  Increasing this value beyond what you need does not yield any performance
  improvements, it simply will block access to that RAM making it unusable by
  your system.

.. list-table:: Common Values
  :widths: 50 25 25
  :header-rows: 1

  * - Resolution
    - Standard Dynamic Range
    - High Dynamic Range (HDR) :ref:`* <libvirt_determining_memory_hdr>`
  * - 1920x1080 (1080p)
    - 32
    - 64
  * - 1920x1200 (1200p)
    - 32
    - 64
  * - 1920x1440 (1440p)
    - 32
    - 64
  * - 3840x2160 (2160p/4K)
    - 128
    - 256

.. _libvirt_determining_memory_hdr:

.. warning::
  While Looking Glass can capture and display HDR, at the time of writing
  neither Xorg or Wayland can make use of it and it will be converted by the
  GPU drivers/hardware to SDR. Additionally using HDR doubles the amount of
  memory, bandwidth, and CPU load and should generally not be used unless you
  have a special reason to do so.

.. _libvirt_shmfile_permissions:

Permissions
~~~~~~~~~~~

The shared memory file used by IVSHMEM is found in ``/dev/shm/looking-glass``.
By default, it is owned by QEMU, and does not give read/write permissions to
your user, which are required for Looking Glass to run properly.

You can use ``systemd-tmpfiles`` to create the file before running your VM,
granting the necessary permissions which allow Looking Glass to use the file
properly.

Create a new file ``/etc/tmpfiles.d/10-looking-glass.conf``, and populate it
with the following::

   # Type Path               Mode UID  GID Age Argument

   f /dev/shm/looking-glass 0660 user kvm -

Change ``UID`` to the user name you will run Looking Glass with, usually your
own.

.. _libvirt_spice_server:

Keyboard/mouse/display/audio
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Looking Glass makes use of the SPICE protocol to provide keyboard and mouse
input, audio input and output, and display fallback.

.. note::
  The default configuration that libvirt uses is not optimal and must be
  adjusted. Failure to perform these changes will cause input issues along
  with failure to support 5 button mice.

If you would like to use SPICE to give you keyboard and mouse input
along with clipboard sync support, make sure you have a
``<graphics type='spice'>`` device, then:

-  Find your ``<video>`` device, and set ``<model type='vga'/>``

   -  If you can't find it, make sure you have a ``<graphics>``
      device, save and edit again.

-  Remove the ``<input type='tablet'/>`` device, if you have one.
-  Create an ``<input type='mouse' bus='virtio'/>`` device, if you don't
   already have one.
-  Create an ``<input type='keyboard' bus='virtio'/>`` device to improve
   keyboard usage.

.. note::
   Be sure to install the the *vioinput* driver from
   `virtio-win <https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/stable-virtio/>`_
   in the guest

To enable audio support add a standard Intel HDA audio device to your
configuration as per below:

.. code:: xml

  <sound model='ich9'>
    <audio id='1'/>
  </sound>
  <audio id='1' type='spice'/>

If you also want clipboard synchronization please see
:ref:`libvirt_clipboard_synchronization`

.. _libvirt_clipboard_synchronization:

Clipboard synchronization
^^^^^^^^^^^^^^^^^^^^^^^^^

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

.. _libvirt_apparmor:

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

.. _libvirt_memballoon_tweak:

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

.. _host_install:

Additional tuning
^^^^^^^^^^^^^^^^^

Looking Glass is latency sensitive and as such it may suffer microstutters if
you have not properly tuned your virtual machine. The physical display output
of your GPU will usually not show such issues due to the nature of the hardware
but be sure that if you are experiencing issues the following tuning is
required to obtain optimal performance.

1. Do not assign all your CPU cores to your guest VM, you must at minimum
   reserve two CPU cores (4 threads) for your host system to use. For example,
   if you have a 6 core CPU, only assign 4 cores (8 threads) to the guest.

2. Ensure you correctly pin your VMs vCPU threads to the correct cores for your
   CPU architecture.

3. If you are on a NUMA architecture (dual CPU, or early Threadripper) be sure
   that you pin the vCPU threads to the physical CPU/die attached to your GPU.

4. Just because your GPU is in a slot that is physically x16 in size, does not
   mean your GPU is running at x16, this is dependent on how your motherboard
   is physically wired and the physical slot may be limited to x4 or x8.

5. Be sure to set your CPU model type to `host-passthrough` so that your guest
   operating system is aware of the acceleration features of your CPU and can
   make full use of them.

6. AMD users be sure that you have the CPU feature flag `topoext` enabled or
   your guest operating system will not be aware of which CPU cores are
   hyper-thread pairs.

7. NVIDIA users may want to enable NvFBC as an alternative capture API in the
   guest. Note that NvFBC is officially available on professional cards only
   and methods to enable NvFBC on non-supported GPUs is against the NVIDIA
   Capture API SDK License Agreement even though GeForce Experience and
   Steam make use of it on any NVIDIA GPU.

How to perform these changes is left as an exercise to the reader.

Host application
----------------

The Looking Glass Host application captures frames from the guest OS using a
capture API, and sends them to the
:ref:`client <client_install>`—be it on the host OS (hypervisor) or another
Virtual Machine—through a low-latency transfer protocol over shared memory.

You can get the host program in two ways:

-  Download a pre-built binary from https://looking-glass.io/downloads
   (**recommended**)

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

To begin, you must first run the Windows VM with the changes noted above in
either the :ref:`libvirt` section.

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

.. _client_install:

Client application
------------------

The Looking Glass client receives frames from the :ref:`host <host_install>` to
display on your screen. It also handles input, and can optionally share the
system clipboard with your guest OS through SPICE.

First you must build the client from source, see :ref:`building`. Once you have
built the client, you can install it. Run the following as root::

   make install

To install for the local user only, run::

   cmake -DCMAKE_INSTALL_PREFIX=~/.local .. && make install
