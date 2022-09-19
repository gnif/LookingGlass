.. _kernel_module:

Kernel module
#############

This kernel module implements a basic interface to the IVSHMEM device
for Looking Glass in VM → VM mode.

Additionally in VM → host mode, it can be used to generate a shared
memory device on the host machine that supports dmabuf.

Prerequisites
-------------

The Linux kernel headers for your kernel version are required for building.
Install them with ``apt-get``

.. code:: bash

   apt-get install linux-headers-$(uname -r)

Then switch to the ``module/`` directory

.. code:: bash

   cd module/

.. _module_dkms:

Using DKMS (recommended)
------------------------

You can use the kernel's DKMS feature to keep the module across upgrades.
``dkms`` must be installed.

.. code:: bash

   apt-get install dkms

.. _module_dkms_install:

Installing
~~~~~~~~~~

To install the module into DKMS, run

.. code:: bash

   dkms install "."

.. _module_dkms_loading:

Loading
~~~~~~~

For VM → VM, simply ``modprobe`` the module::

   modprobe kvmfr

For VM → host with dmabuf, ``modprobe`` with the parameter
``static_size_mb``:

.. code:: bash

   modprobe kvmfr static_size_mb=32

Just like above, multiple devices can be created by separating the sizes
with commas.


.. _module_manual:

Compiling & loading (manual)
----------------------------

To compile the module manually, run ``make`` in the module directory.

.. _module_manual_loading:

Loading
~~~~~~~

For VM → VM mode, run:

.. code:: bash

   insmod kvmfr.ko

For VM → host mode with dmabuf, instead of creating a shared memory file,
load this module with the parameter ``static_size_mb``. For example, a
32 MiB shared memory device can be created with:

.. code:: bash

   insmod kvmfr.ko static_size_mb=32

Multiple devices can be created by separating the sizes with commas. For
example, ``static_size_mb=128,64`` would create two kvmfr devices:
``kvmfr0`` would be 128 MB and ``kvmfr1`` would be 64 MiB.

.. note::

   If you have already loaded an older version of the module, unload it
   first. You can do this by rebooting, or with ``rmmod``:

   .. code:: bash

      rmmod kvmfr.ko

.. _module_usage:

Usage
-----

The module will create the ``/dev/kvmfr0`` node, which represents the KVMFR
interface. To use the interface, you need permission to access it by
either: creating a udev rule to ensure your user can read and write to
it, or simply change its ownership manually, i.e.:

.. code:: bash

   sudo chown user:user /dev/kvmfr0

As an example, you can create a new file in ``/etc/udev/rules.d/99-kvmfr.rules``
with the following contents::

   SUBSYSTEM=="kvmfr", OWNER="user", GROUP="kvm", MODE="0660"

(replace ``user`` with your username)

Usage with Looking Glass is simple, you only need to specify the path to
the device node, for example:

.. code:: bash

   ./looking-glass-client -f /dev/kvmfr0

You may also use a config file: ``~/.looking-glass-client.ini``, or
``/etc/looking-glass-client.ini``.

.. code:: ini

   [app]
   shmFile=/dev/kvmfr0

.. _module_vm_to_host:

VM → Host
~~~~~~~~~~~~

In VM → host mode, use this device in place of the shared memory file.

QEMU
^^^^

Add the following arguments to your ``qemu`` command line::

   -device ivshmem-plain,id=shmem0,memdev=looking-glass
   -object memory-backend-file,id=looking-glass,mem-path=/dev/kvmfr0,size=32M,share=yes

.. note::

   The ``size`` argument must be the same size you passed
   to the ``static_size_mb`` argument when loading the kernel module.

libvirt
^^^^^^^

Starting with QEMU 6.2 and libvirt 7.9, JSON style QEMU configuration is the
default syntax. Users running QEMU 6.2 or later **and** libvirt 7.9 or later,
should use this XML block to configure their VM for kvmfr:

.. code:: xml

   <qemu:commandline>
     <qemu:arg value='-device'/>
     <qemu:arg value='{"driver":"ivshmem-plain","id":"shmem0","memdev":"looking-glass"}'/>
     <qemu:arg value='-object'/>
     <qemu:arg value='{"qom-type":"memory-backend-file","id":"looking-glass","mem-path":"/dev/kvmfr0","size":33554432,"share":true}'/>
   </qemu:commandline>

.. note::

   -  The ``"size"`` tag represents the size of the shared memory device in
      bytes. Once you determine the proper size of the device as per
      :ref:`Determining Memory <libvirt_determining_memory>`, use the figure you
      got to calculate the size in bytes:

     ``size_in_MB x 1024 x 1024 = size_in_bytes``

If you are running QEMU older than 6.2 or libvirt older than 7.9, please use
legacy syntax for IVSHMEM setup:

.. code:: xml

   <qemu:commandline>
     <qemu:arg value='-device'/>
     <qemu:arg value='ivshmem-plain,id=shmem0,memdev=looking-glass'/>
     <qemu:arg value='-object'/>
     <qemu:arg value='memory-backend-file,id=looking-glass,mem-path=/dev/kvmfr0,size=32M,share=yes'/>
   </qemu:commandline>

.. note::

   -  Using the legacy syntax on QEMU 6.2/libvirt 7.9 may cause QEMU to
      abort with the following error message:
      "``error: internal error: ... PCI: slot 1 function 0 not available for pcie-root-port, in use by ivshmem-plain``"

   -  Remember to add ``xmlns:qemu='http://libvirt.org/schemas/domain/qemu/1.0'``
      to the ``<domain>`` tag.

Running libvirt this way violates AppArmor and cgroups policies, which will
block the VM from running. These policies must be amended to allow the VM
to start:

- For AppArmor, create ``/etc/apparmor.d/local/abstractions/libvirt-qemu`` if
  it doesn't exist, and add the following::

     # Looking Glass
     /dev/kvmfr0 rw,

- For cgroups, edit ``/etc/libvirt/qemu.conf``, uncomment the
  ``cgroup_device_acl`` block, and add ``/dev/kvmfr0`` to the list.
  Then restart ``libvirtd``:

  .. code:: bash

   sudo systemctl restart libvirtd.service

.. _systemd_modules_load:

systemd-modules-load
~~~~~~~~~~~~~~~~~~~~

For convenience, you may load the KVMFR module when starting your computer.
We can use the ``systemd-modules-load.service(8)`` service for this task.

Create the file ``/etc/modules-load.d/kvmfr.conf`` with the following
contents::

   # KVMFR Looking Glass module
   kvmfr

This will now run the next time you start your machine.

If you are running in VM → host mode, you must additionally create another file
``/etc/modprobe.d/kvmfr.conf`` to properly set the size. It should have the
following contents::

   #KVMFR Looking Glass module
   options kvmfr static_size_mb=32

.. note::

   Don't forget to adjust ``static_size_mb`` to your needs.
