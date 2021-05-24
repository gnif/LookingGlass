Kernel Module
#############

This kernel module implements a basic interface to the IVSHMEM device
for LookingGlass when using LookingGlass in VM->VM mode.

Additionally, in VM->host mode, it can be used to generate a shared
memory device on the host machine that supports dmabuf.

Compiling (Manual)
------------------

Make sure you have your kernel headers installed first, on Debian/Ubuntu
use the following command::

   apt-get install linux-headers-$(uname -r)

Then simply run ``make`` and you're done.

Loading
~~~~~~~

For VM->VM mode, simply run::

   insmod kvmfr.ko

For VM->host mode with dmabuf, instead of creating a shared memory file,
load this module with the parameter ``static_size_mb``. For example, a
128 MB shared memory device can be created with::

   insmod kvmfr.ko static_size_mb=128

Multiple devices can be created by separating the sizes with commas. For
example, ``static_size_mb=128,64`` would create two kvmfr devices:
``kvmfr0`` would be 128 MB and ``kvmfr1`` would be 64 MB.

.. _compiling--installing-dkms:

Compiling & Installing (DKMS)
-----------------------------

You can install this module into DKMS so that it persists across kernel
upgrades. Simply run::

   dkms install .

.. _loading-1:

Loading
~~~~~~~

For VM->VM, simply modprobe the module::

   modprobe kvmfr

For VM->host with dmabuf, modprobe with the parameter
``static_size_mb``::

   modprobe kvmfr static_size_mb=128

Just like above, multiple devices can be created by separating the sizes
with commas.

Usage
-----

This will create the ``/dev/kvmfr0`` node that represents the KVMFR
interface. To use the interface you need permission to access it by
either creating a udev rule to ensure your user can read and write to
it, or simply change its ownership manually, ie::

   sudo chown user:user /dev/kvmfr0

An example udev rule, which you can put in
``/etc/udev/rules.d/99-kvmfr.rules``, is (replace ``user`` with your
username)::

   SUBSYSTEM=="kvmfr", OWNER="user", GROUP="kvm", MODE="0660"

Usage with looking glass is simple, you only need to specify the path to
the device node, for example::

   ./looking-glass-client -f /dev/kvmfr0

You may also use a config file: ``~/.looking-glass-client.ini``, or
``/etc/looking-glass-client.ini``.

.. code:: ini

   [app]
   shmFile=/dev/kvmfr0

VM->Host
~~~~~~~~

In VM->host mode, use this device in place of the shared memory file.

For example, with ``qemu``, you would use the following arguments::

   -device ivshmem-plain,id=shmem0,memdev=looking-glass
   -object memory-backend-file,id=looking-glass,mem-path=/dev/kvmfr0,size=128M,share=yes

Note that the ``size`` argument must be the same size as what you passed
to ``static_size_mb`` argument for the kernel module.

``libvirt``
^^^^^^^^^^^

With ``libvirt``, you can use the following XML block:

.. code:: xml

   <qemu:commandline>
     <qemu:arg value='-device'/>
     <qemu:arg value='ivshmem-plain,id=shmem0,memdev=looking-glass'/>
     <qemu:arg value='-object'/>
     <qemu:arg value='memory-backend-file,id=looking-glass,mem-path=/dev/kvmfr0,size=128M,share=yes'/>
   </qemu:commandline>

Remember to add
``xmlns:qemu='http://libvirt.org/schemas/domain/qemu/1.0'`` to the
``<domain>``.

On certain distros, running libvirt this way poses issues with apparmor
and cgroups.

For apparmor, create ``/etc/apparmor.d/local/abstractions/libvirt-qemu`` if
it doesn't exist, and add the following::

   # Looking Glass
   /dev/kvmfr0 rw,

For cgroups, in ``/etc/libvirt/qemu.conf``, uncomment the
``cgroup_device_acl`` block and add ``/dev/kvmfr0`` to the list. Then
restart ``libvirtd``::

   sudo systemctl restart libvirtd.service

.. _systemd_modules_load:

systemd-modules-load
~~~~~~~~~~~~~~~~~~~~

For convenience, you may load the KVMFR module when starting your computer.
We can use the ``systemd-modules-load.service(8)`` service for this task.

Create a file as ``/etc/modules-load.d/kvmfr.conf`` with the following
contents::

   #KVMFR Looking Glass module
   kvmfr

This will now run the next time you start your machine.

If you are running in VM->host mode, you must additionally add another file in
``/etc/modprobe.d/kvmfr.conf`` to properly set the size. It should have the
following contents, while adjusting ``static_size_mb`` to your needs::

   #KVMFR Looking Glass module
   options kvmfr static_size_mb=128
