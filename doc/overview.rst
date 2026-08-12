.. _overview:

Overview
########

Looking Glass displays a Windows virtual machine in a low-latency Linux
window. It transfers completed frames through shared memory instead of sending
compressed video over a network. The current IDD provides video and direct
input through LGMP, while SPICE normally provides audio, clipboard and fallback
services.

The names used throughout this guide are:

Linux host
   The physical machine running KVM/QEMU and the Looking Glass Client.

Windows guest
   The Windows virtual machine shown by Looking Glass.

Looking Glass Client
   The Linux application that displays the guest and sends user input. There
   are currently no distribution packages for it, so it must be built from
   source before installation.

Looking Glass Server
   The Windows guest application that generates the video feed for the Looking
   Glass client. There are broadly two implementations of the server: the
   Looking Glass IDD and the legacy host application.

Looking Glass IDD
   The recommended Windows Indirect Display Driver. It creates a virtual
   monitor, sends its frames to the client and provides direct keyboard and
   mouse input. The installer also includes the IDD helper and input driver.

Legacy Host Application
   The older Windows capture application. It captures an existing display
   rather than creating a virtual one. It is documented for existing B7
   workflows, but is no longer recommended for current installations.

KVMFR and IVSHMEM
   The shared-memory path between the Windows guest and Linux host. KVMFR is
   the recommended Linux kernel module because it permits direct GPU imports
   where supported.

LGMP
   The protocol used by Looking Glass components over shared memory.

SPICE
   An optional fallback transport for video, input, audio and clipboard
   services. With the IDD active, Looking Glass normally uses its direct input
   path and uses SPICE only for services that are enabled and available.

Recommended setup
-----------------

For a new installation, use:

* the current Looking Glass Client on the Linux host;
* KVMFR shared memory attached to the Windows guest;
* the matching Looking Glass IDD in the Windows guest; and
* SPICE only for the fallback or convenience services you need.

Use matching Looking Glass releases for the current client, IDD, and OBS plugin.
The shared-memory protocol changes between releases and incompatible components
will not connect. Users of the legacy host application must use the complete
matching B7 stack described below.

.. _legacy_host_policy:

Legacy Host Application
-----------------------

The Host Application is the legacy implementation of the Looking Glass server.
The IDD is recommended because it does not require a physical monitor or dummy
plug and supports current display, input and scheduling features.

If your workflow specifically requires non-capture mouse input with the Host
Application, B7 is the last recommended release. Use the matching B7 client,
Host Application and B7 documentation together; do not mix B7 components with
current releases.

Download the complete B7 release from https://looking-glass.io/downloads and see
the :ref:`legacy_host` section.
