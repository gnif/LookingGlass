:orphan:
.. _ivshmem_kvmfr:

IVSHMEM with the KVMFR module (Recommended)
###########################################

The kernel module implements a basic interface to the IVSHMEM device
for Looking Glass allowing DMA GPU transfers.

.. _ivshmem_kvmfr_prereq:

Prerequisites
-------------

The Linux kernel headers for your kernel version are required for building
along with `dkms` to manage the module when you upgrade your kernel.

.. code:: bash

   apt-get install linux-headers-$(uname -r) dkms

Then switch to the ``module/`` directory

.. code:: bash

   cd module/

.. _ivshmem_kvmfr_dkms:

Installing
~~~~~~~~~~

To install the module into DKMS, run

.. code:: bash

   dkms install "."

.. _ivshmem_kvmfr_loading:

Loading
~~~~~~~

Using the value you should have already calculated as per
:ref:`Determining Memory <libvirt_determining_memory>`, simply use
``modprobe`` with the parameter ``static_size_mb``, for example:

.. code:: bash

   modprobe kvmfr static_size_mb=32

Alternatively you can make this setting permanent by creating the file
``/etc/modprobe.d/kvmfr.conf`` with the following content.

.. code:: text

   options kvmfr static_size_mb=32

After this has been done, simply running ``modprobe kvmfr`` is all that is
required.

.. note::

   Don't forget to adjust ``static_size_mb`` to your needs.

.. _ivshmem_kvmfr_systemd:

systemd-modules-load
~~~~~~~~~~~~~~~~~~~~

For convenience, you may load the KVMFR module when starting your computer.
We can use the ``systemd-modules-load.service(8)`` service for this task.

Create the file ``/etc/modules-load.d/kvmfr.conf`` with the following
contents::

   # KVMFR Looking Glass module
   kvmfr

This will now run the next time you start your machine.

.. _ivshmem_kvmfr_verification:

Verification
~~~~~~~~~~~~

If everything has been done correctly you should see the following output in
dmesg:

.. code:: text

   kvmfr: creating 1 static devices

You should now also have the character device ``/dev/kvmfr0``

.. code:: bash

   $ ls -l /dev/kvmfr0
   crw------- 1 root root 242, 0 Mar  5 05:53 /dev/kvmfr0

.. warning::

   If you start the VM prior to loading the module, QEMU will create the file
   ``/dev/kvmfr0`` as a regular file. You can confirm if this has happened by
   running ``ls -l /dev/kvmfr0`` and checking if the file size is greater then
   zero, or the permissions do not start with ``c``. If this has occurred, you
   must delete the file and reload the module.

.. _ivhsmem_kvmfr_permissions:

Permissions
~~~~~~~~~~~

The module will create the ``/dev/kvmfr0`` node, which represents the KVMFR
interface. To use the interface, you need permission to access it by either
creating a udev rule to ensure your user can read and write to it, or simply
change its ownership manually, i.e.:

.. code:: bash

   sudo chown user:kvm /dev/kvmfr0

As an example, you can create a new file in ``/etc/udev/rules.d/99-kvmfr.rules``
with the following contents::

   SUBSYSTEM=="kvmfr", OWNER="user", GROUP="kvm", MODE="0660"

(replace ``user`` with your username)

.. _ivshmem_kvmfr_libvirt:

libvirt
~~~~~~~

Using the module in libvirt requires adding a ``<qemu:commandline>`` block to
your libvirt XML configuration. That block, in turn, requires modifying the
XML domain namespace. To modify the namespace edit the ``<domain>`` tag at
the top of your XML config to:

.. code:: xml

   <domain type='kvm' xmlns:qemu='http://libvirt.org/schemas/domain/qemu/1.0'>

then add one of the ``<qemu:commandline>`` blocks below based on your
QEMU/libvirt versions.

.. note::

   Make sure to add both the block and the domain namespace change in a
   single editing session prior to saving it. Failure to do so will cause
   libvirt to reject the changes.

Starting with QEMU 6.2 and libvirt 7.9, JSON style QEMU configuration is the
default syntax. Users running QEMU 6.2 or later **and** libvirt 7.9 or later,
should use this XML block to configure their VM for kvmfr:

.. code:: xml

   <qemu:commandline>
     <qemu:arg value="-device"/>
     <qemu:arg value="{'driver':'ivshmem-plain','id':'shmem0','memdev':'looking-glass'}"/>
     <qemu:arg value="-object"/>
     <qemu:arg value="{'qom-type':'memory-backend-file','id':'looking-glass','mem-path':'/dev/kvmfr0','size':33554432,'share':true}"/>
   </qemu:commandline>

.. note::

   -  The ``'size'`` tag represents the size of the shared memory device in
      bytes. Once you determine the proper size of the device as per
      :ref:`Determining Memory <libvirt_determining_memory>`, use the figure you
      got to calculate the size in bytes:

     ``size_in_MB x 1024 x 1024 = size_in_bytes``

If you are running QEMU older than 6.2 or libvirt older than 7.9, please use
legacy syntax for IVSHMEM setup:

.. code:: xml

   <qemu:commandline>
     <qemu:arg value="-device"/>
     <qemu:arg value="ivshmem-plain,id=shmem0,memdev=looking-glass"/>
     <qemu:arg value="-object"/>
     <qemu:arg value="memory-backend-file,id=looking-glass,mem-path=/dev/kvmfr0,size=32M,share=yes"/>
   </qemu:commandline>

.. note::

   Using the legacy syntax on QEMU 6.2/libvirt 7.9 may cause QEMU to
   abort with the following error message:
   "``error: internal error: ... PCI: slot 1 function 0 not available for pcie-root-port, in use by ivshmem-plain``"

Running libvirt this way violates AppArmor and cgroups policies, which will
block the VM from running. These policies must be amended to allow the VM
to start.

.. tip::

   If you are not sure, you likely have cgroups also as this is usually deployed
   and configured by default by most distributions when you install libvirt.

AppArmor
^^^^^^^^

Create ``/etc/apparmor.d/local/abstractions/libvirt-qemu`` if it doesn't exist
and add the following:

.. code:: text

   # Looking Glass
   /dev/kvmfr0 rw,

cgroups
^^^^^^^

Edit the file ``/etc/libvirt/qemu.conf`` and uncomment the ``cgroup_device_acl``
block, adding ``/dev/kvmfr0`` to the list. To make this change active you then
must restart ``libvirtd``

.. code:: bash

   sudo systemctl restart libvirtd.service

.. _ivshmem_kvmfr_qemu:

QEMU
~~~~

If you are using QEMU directly without libvirt, add the following arguments to your
``qemu`` command line::

   -device ivshmem-plain,id=shmem0,memdev=looking-glass
   -object memory-backend-file,id=looking-glass,mem-path=/dev/kvmfr0,size=32M,share=yes

.. note::

   The ``size`` argument must be the same size you passed
   to the ``static_size_mb`` argument when loading the kernel module.

