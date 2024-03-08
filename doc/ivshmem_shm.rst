:orphan:
.. _ivshmem_shm:

IVSHMEM with standard shared memory
###################################

This method is here for those that can not use the KVMFR kernel module. Please
be aware that as a result you will not be able to take advantage of your GPUs
ability to access memory via it's hardware DMA engine if you use this method.

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
