.. _installing_libvirt:

libvirt/QEMU Installation
#########################

This article assumes you already have a working `libvirt` Windows virtual
machine. If you use `virt-manager`, this guide also applies because
`virt-manager` uses `libvirt` as its back end. A passed-through or virtual GPU
is strongly recommended, but the IDD can start in software mode without one.

.. _libvirt_determining_memory:

Determining memory
^^^^^^^^^^^^^^^^^^

Calculate the base IVSHMEM requirement as:

.. math::

  \text{BASE SIZE} =
  \left(\left\lceil\frac{\text{WIDTH} \times 4}{256}\right\rceil
  \times 256\right) \times \text{HEIGHT} \times 3

Additional shared memory is required for protocol metadata and alignment. The
values below include that requirement and are rounded up to a power of two.

If a configured mode does not fit, the IDD omits it from the Windows mode list.
If a client-requested dynamic resolution does not fit, the helper refuses it
and reports the minimum power-of-two size to configure. The current IDD does
not publish a truncated frame.

.. note::
   Increasing this value beyond what you need does not improve performance. It
   only reserves more host RAM for the VM.

.. list-table:: Common IDD values
   :widths: 60 40
   :header-rows: 1

   * - Maximum resolution
     - Total IVSHMEM size (MiB)
   * - 1920x1080 (1080p)
     - 64
   * - 1920x1200
     - 64
   * - 2560x1440 (1440p)
     - 128
   * - 3440x1440
     - 128
   * - 3840x2160 (4K)
     - 256
   * - 5120x1440
     - 128
   * - 5120x2880 (5K)
     - 256
   * - 7680x4320 (8K)
     - 512

.. _libvirt_determining_memory_hdr:

HDR uses the same 32-bit IDD transport allocation as SDR, so it does not double
the IVSHMEM requirement. Native Linux HDR presentation has separate compositor
and display requirements; see :ref:`client_hdr`.

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

Keyboard/mouse/display/audio
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The IDD provides the primary video, keyboard and mouse paths directly over
LGMP. SPICE is optional, but is recommended for display fallback, clipboard
and audio services.

.. note::
  The current client owns its SPICE connection and service settings. The
  canonical options are ``spice:enable``, ``spice:input``,
  ``spice:clipboard``, ``spice:audio`` and ``spice:usbAudio``.

Keep a ``<graphics type='spice'>`` device if you want these services. For a
usable display fallback, set the VM's ``<video>`` model to ``vga``.

The direct IDD input path supports absolute desktop positioning, relative
capture-mode input, keyboards, media keys and extended mouse buttons. SPICE
fallback uses the VM's default PS/2 keyboard and mouse. Looking Glass does not
require additional virtio keyboard, mouse or tablet devices for either path.

For classic SPICE audio, add a standard Intel HDA audio device:

.. code:: xml

  <sound model='ich9'>
    <audio id='1'/>
  </sound>
  <audio id='1' type='spice'/>

The recommended emulated USB audio path does not need this HDA device. It
requires a SPICE USB channel and is covered in :ref:`client_usb_audio`. For
clipboard synchronization, continue with
:ref:`libvirt_clipboard_synchronization`.

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
