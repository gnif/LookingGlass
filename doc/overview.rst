.. _overview:

Overview
########

Looking Glass displays a Windows virtual machine in a low-latency Linux
window. It transfers completed frames through shared memory instead of sending
compressed video over a network. The IDD provides video and direct
input through LGMP, while SPICE provides audio, clipboard, and fallback
services.

.. _documentation:

Documentation
-------------

.. _docs_distro:

Linux distribution
~~~~~~~~~~~~~~~~~~

The documentation is targeted at Debian users on the stable release channel
(currently |debian_stable|). Newer unstable release channels and
Debian-derivative distributions should also be well served, but some specifics
may be different. Looking Glass is supported on any distribution that provides
the :ref:`required dependencies <client_dependencies>` and meets the
:ref:`minimum requirements <requirements>`, but specific package names within
the documentation may not apply.

.. _docs_terms:

Terms
~~~~~

The terms used throughout the documentation are:

Linux host
   The physical machine running KVM/QEMU and the Looking Glass Client.

Windows guest
   The Windows virtual machine shown by Looking Glass.

Looking Glass Client
   The Linux application that displays video from the guest, relays audio, and
   sends user input.

Looking Glass Server
   The Windows guest application that generates the video feed for the Looking
   Glass client. There are broadly two implementations of the server: the
   Looking Glass IDD and the legacy host application.

Looking Glass IDD
   The recommended Windows Indirect Display Driver. It creates a virtual
   monitor, sends its frames to the client and provides direct keyboard and
   mouse input.

Legacy Host Application
   The older Windows capture application. It captures an existing display
   rather than creating a virtual one. It is documented for existing B7
   workflows, but is no longer recommended for current installations.

KVMFR and IVSHMEM
   The shared-memory path between the Windows guest and Linux host. KVMFR is
   the recommended Linux kernel module, as it permits direct GPU imports where
   supported.

LGMP
   The protocol used by Looking Glass components over shared memory.

SPICE
   Remote communications protocol for virtual machines. Used by Looking Glass
   for its emulated USB audio device. Also serves as an optional fallback
   transport for video, input, audio and clipboard services.

.. _recommended_setup:

Recommended setup
-----------------

For a new installation, use:

* the current Looking Glass Client on the Linux host;
* KVMFR shared memory attached to the Windows guest;
* the current Looking Glass IDD in the Windows guest; and
* SPICE installed on the Linux host

Make sure to use the same version of Looking Glass for the client, IDD, and OBS
plugin. The shared-memory protocol changes between releases and incompatible
components are unable to connect. Users of the Legacy Host Application must use
the complete B7 stack described below.

.. _legacy_host_policy:

Legacy Host Application
-----------------------

The Legacy Host Application is the legacy implementation of the Looking Glass
server. The IDD is recommended, because it does not require a physical monitor
or dummy plug, and supports current display, input, and scheduling features.

If your workflow specifically requires non-capture mouse input with the Host
Application, B7 is the last recommended release. Use the B7 client, Host
Application and B7 documentation together; do not mix B7 components with current
releases.

Download the complete B7 release from https://looking-glass.io/downloads and see
the :ref:`legacy_host` section.

