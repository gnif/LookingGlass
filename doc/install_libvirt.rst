.. _installing_libvirt:

libvirt/QEMU Installation
#########################

This article assumes you already have a working `libvirt` Windows virtual
machine. It also applies to users of `virt-manager`, because `virt-manager` uses
`libvirt` as its back end. A passed-through or virtual GPU is strongly
recommended, but the IDD can start in software mode without one.

.. _libvirt_determining_memory:

Determining memory
^^^^^^^^^^^^^^^^^^

Calculate the IVSHMEM requirement as:

.. math::

  \text{REQUIRED BYTES} =
  \left(\left\lceil\frac{\text{WIDTH} \times 4}{256}\right\rceil
  \times 256\right) \times \text{HEIGHT} \times 3
  + (4 \times 1024 \times 1024)

Convert the result to MiB and round it up to the next power of two. Common
values listed below have already been rounded.

If a configured mode does not fit, the IDD omits it from the Windows mode list.
If a client-requested dynamic resolution does not fit, the helper refuses it
and reports the minimum power-of-two size to configure. The IDD will not serve a
truncated frame.

.. note::
   Increasing this value beyond what you need does not improve performance. It
   only reserves more host RAM for the VM.

.. list-table:: Common IDD values
   :widths: 60 40
   :header-rows: 1

   * - Maximum resolution
     - Total IVSHMEM size (MiB)
   * - 1920x1080 (1080p)
     - 32
   * - 1920x1200
     - 32
   * - 2560x1440 (1440p)
     - 64
   * - 3440x1440
     - 64
   * - 3840x2160 (4K)
     - 128
   * - 5120x1440
     - 128
   * - 5120x2880 (5K)
     - 256
   * - 7680x4320 (8K)
     - 512

.. _libvirt_determining_memory_hdr:

.. admonition:: HDR Requirements

   HDR uses the same 32-bit IDD transport allocation as SDR, so it does not
   double the IVSHMEM requirement. Native Linux HDR presentation has separate
   compositor and display requirements; see :ref:`client_hdr`.

.. _libvirt_ivshmem:

IVSHMEM
^^^^^^^

There are two methods of configuring IVSHMEM, using shared memory directly, or
using the KVMFR kernel module. While the KVMFR module is slightly more
complicated to configure, it substantially improves performance as it allows
Looking Glass to use your GPUs DMA engine to transfer the frame data.

.. toctree::
   :maxdepth: 1

   ivshmem_kvmfr
   ivshmem_shm

.. seealso::

   :ref:`igpu_kvmfr_recommended`


.. _libvirt_spice_server:

SPICE fallback server
^^^^^^^^^^^^^^^^^^^^^

The Looking Glass client also supports SPICE for video, audio, and input if
there is a problem with the Looking Glass IDD. However, this is no faster than
other SPICE clients like virt-viewer, and should not normally be used.

.. note::

  The SPICE server is required for the USB audio device, as it uses
  SPICE USB redirection channels.

  See :ref:`client_usb_audio` for more information.

To enable SPICE support, add a SPICE graphics server to your virtual machine:

.. code:: xml

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

   /dev/shm/looking-glass rw,

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

.. _libvirt_additional_tuning:

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

How to perform these changes is left as an exercise to the reader.
