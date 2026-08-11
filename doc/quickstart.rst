.. _quick_start:

Quick start
###########

This is the shortest path to a current Looking Glass installation. It assumes
that KVM/QEMU and the Windows guest are already working.

1. :doc:`Check the requirements <requirements>`.
2. :ref:`Download and build the Linux client <build_client_section>`. Current
   releases are not provided as Linux distribution packages.
3. :doc:`Configure KVMFR and IVSHMEM <install_libvirt>` for the largest guest
   resolution you intend to use.
4. :doc:`Install the matching Looking Glass IDD <install_idd>` inside the
   Windows guest. Install the bundled IVSHMEM driver when required.
5. :doc:`Install the client <install_client>` from its build directory.
6. Start the Windows guest, then run:

   .. code:: bash

      looking-glass-client

The client selects ``/dev/kvmfr0`` automatically when it is present, and
otherwise uses ``/dev/shm/looking-glass``. If your path differs, select it with
``-f`` or ``lgmp:shmDevice``.

The first connection should show the virtual Looking Glass display created by
the IDD. If it does not, follow :doc:`the no-display checks
<troubleshooting>` before changing performance settings.

.. note::

   SPICE is optional for the primary IDD video and input paths. Keep it enabled
   if you want clipboard, audio or display fallback services.
