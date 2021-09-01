Technical FAQ
#############

This FAQ is targeted at developers or technical people that want to
know more about what's going on under the hood.

.. _ivshmemshared_ram:

IVSHMEM/Shared RAM
------------------

.. _what_exactly_is_the_ivshmem_device:

What exactly is the IVSHMEM device?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

This is a virtual device that maps a segment of shared memory into the
guest via a BAR (Base Address Register). It also has additional features
such as interrupt triggering for synchronization however we do not use
these.

.. _what_is_the_ivshmem_device_being_used_for:

What is the IVSHMEM device being used for?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One might assume that we are simply using the device for the captured
frames, this, however, is not entirely accurate. Looking Glass also
needs to capture mouse shape changes (the mouse cursor), and mouse
movement events and feed these back to the client to render. We need
this additional information as we actually are rendering the cursor on
the client-side, independent of the frame capture. This is why when you
move your cursor around it doesn't affect the UPS, which is only
counting frame updates.

.. _why_do_you_need_the_mouse_positional_information:

Why do you need the mouse positional information?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Windows has no notion of an absolute pointing device unless you are
using a tablet, which does work, however, if you also want relative
input for applications/games that require cursor capture, you need a
relative input device such as a PS/2 mouse.

The problem is, due to the design of QEMU or the Windows mouse subsystem
(not sure which), when the VM has both devices attached (which is the
default for libvirt), mouse click events are always at the last location
of the absolute positional device (tablet) even if the cursor has been
moved with the relative input device.

Because of this bug, we need to always operate in relative mouse input
mode, and since factors like windows mouse acceleration, or cursor
movement by a user application may occur in the guest, we need to pass
this information back so the client can render the cursor in the correct
location.

.. _why_does_lg_poll_for_updates_instead_of_using_interrupts:

Why does LG poll for updates instead of using interrupts?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Initially, we were using interrupts in early designs however it became
clear that the performance, especially for high update rate mice was
extremely poor. This may have improved in recent QEMU versions and
perhaps should be re-evaluated at some point.
