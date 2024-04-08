.. _installing_libvirt:

libvirt/QEMU Installation
#########################

This article assumes you already have a fully functional `libvirt` domain with
PCI passthrough working. If you use `virt-manager`, this guide also applies to
you, since virt-manager uses `libvirt` as its back end.

.. _libvirt_determining_memory:

Determining memory
^^^^^^^^^^^^^^^^^^

You will first need to calculate the memory size to be suitable for your desired
maximum resolution using the following formula:

.. math::

  \text{WIDTH} \times \text{HEIGHT} \times \text{BPP} \times 2 = \text{frame size in bytes}

  \text{frame size in bytes} \div 1024 \div 1024 = \text{ frame size in MiB}

  \text{frame size in MiB} + 10 = \text{ required size in MiB}

  2^{\lceil \log_2(\text {required size in MiB}) \rceil} = \text{ total MiB}

Where `BPP` is 4 for 32-bit RGB (SDR) or 8 for 64-bit
(HDR :ref:`* <libvirt_determining_memory_hdr>`).

.. hint::
  The final step in this calculation is simply rounding the value up to the
  nearest power of two.

For example, for a resolution of 1920x1080 (1080p) SDR:

.. math::

  1920 \times 1080 \times 4 \times 2 = 16,588,800 \text{ bytes}

  16,588,800 \div 1024 \div 1024 = 15.82 \text{ MiB}

  15.82 \text{ MiB} + 10 \text{ MiB} = 25.82 \text{ MiB}

  2^{\lceil \log_2(25.82) \rceil} = 32 \text { MiB}


Failure to provide enough memory will cause Looking Glass to truncate the
bottom of the screen and will trigger a message popup to inform you of the size
you need to increase the value to.

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
  * - 2560x1440 (1440p)
    - 64
    - 128
  * - 3840x2160 (2160p/4K)
    - 128
    - 256

.. _libvirt_determining_memory_hdr:

.. warning::
  While Looking Glass can capture and display HDR, at the time of writing
  neither Xorg or Wayland can make use of it and it will be converted by the
  GPU drivers/hardware to SDR. Additionally using HDR doubles the amount of
  memory, bandwidth, and CPU load and as such should generally not be used
  unless you have a special reason to do so.

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

How to perform these changes is left as an exercise to the reader.
