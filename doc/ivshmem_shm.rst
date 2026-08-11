:orphan:
.. _ivshmem_shm:

IVSHMEM with standard shared memory
###################################

Use this method only when the KVMFR kernel module is unavailable. A standard
shared-memory file cannot provide KVMFR's DMA-BUF export, so the client must
copy the frame before uploading it to the host GPU.

Add the following to your libvirt machine configuration inside the
'devices' section by running ``virsh edit <VM>`` where ``<VM>`` is the name of
your virtual machine.

.. code:: xml

   <shmem name='looking-glass'>
     <model type='ivshmem-plain'/>
     <size unit='M'>64</size>
   </shmem>

.. note::
  If you are using QEMU directly without libvirt the following arguments are
  required instead.

  Add the following to the commands to your QEMU command line, adjusting
  the ``bus`` parameter to suit your particular configuration:

  .. code:: bash

     -device ivshmem-plain,memdev=ivshmem,bus=pcie.0 \
     -object memory-backend-file,id=ivshmem,share=on,mem-path=/dev/shm/looking-glass,size=64M

The example uses 64 MiB. Replace it with the value from
:ref:`Determining memory <libvirt_determining_memory>` when using a larger
resolution.

.. warning::
   Stop the VM before changing this size. You may need to remove the existing
   ``/dev/shm/looking-glass`` file so QEMU can recreate it at the new size.
   Check its permissions afterward; see :ref:`libvirt_shmfile_permissions`.

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
