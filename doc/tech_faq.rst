Technical notes
###############

This page explains a few implementation choices. It is not required for normal
installation.

.. _ivshmemshared_ram:

IVSHMEM and shared memory
-------------------------

.. _what_exactly_is_the_ivshmem_device:

What is the IVSHMEM device?
~~~~~~~~~~~~~~~~~~~~~~~~~~~

IVSHMEM maps the same reserved memory into QEMU, the Windows guest and Linux.
KVMFR provides the Linux character-device interface used by the client and OBS
and can export regions for direct GPU import.

The IDD stores frame queues, frame metadata, pointer updates and input protocol
state in this region. It uses three frame buffers so a producer and multiple
consumers can progress without overwriting a frame that is still in use.

.. _what_is_the_ivshmem_device_being_used_for:

Why is the allocation larger than one image?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In addition to three aligned images, the region contains LGMP queues and GPU
resource alignment. Use :ref:`libvirt_determining_memory` rather than
multiplying width and height once.

.. _why_do_you_need_the_mouse_positional_information:

Why are there both absolute and relative mouse paths?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Desktop interaction needs absolute positioning so the host and guest pointers
stay aligned without capture. Games that lock the cursor need unbounded
relative movement. The IDD exposes both and the client selects the appropriate
path when capture mode changes.

SPICE fallback remains relative-only. Pointer position messages are also used
to render the guest cursor independently from desktop frame updates.

.. _why_does_lg_poll_for_updates_instead_of_using_interrupts:

Why does Looking Glass poll for updates?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Polling avoids the high overhead and batching behavior seen with virtual
interrupts, especially for high-rate pointer updates. The polling intervals
are configurable for diagnosis, but lowering them without evidence increases
CPU usage and does not guarantee lower latency.

How does cadence reduce bandwidth?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Clients publish their presentation period and deadline in the shared protocol.
The IDD prepares guest frames as they arrive, but transports only the newest
frame needed for the fastest active consumer. Other consumers independently
select the newest frame appropriate for their own rate. Clock-domain feedback
is exchanged as periods and deadlines rather than comparing raw timestamps
from Windows and Linux.
