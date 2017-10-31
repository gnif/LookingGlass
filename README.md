KVMGFX
======

Purpose
-------

To provide a low latency KVM Guest client that relies on video capture rather
than the output of a virtual VGA device. The goal of this project is to allow
lossless zero latency display of a guest on the host on the local host.

The Problem
-----------

Exising solutions such as Spice are designed to work with a virtual VGA
adaptor, but when a physical VGA device is passed into the guest VM the
display can not be presented to the host, instead a physical monitor must be
attached to the physcal VGA device. While this may be acceptible to some it
requires a physical switch between the host and guest VM preventing the VM
from being inlined into the host window manager.

Existing Solutions
------------------

Some have reported that they are able to stream using Steam InHouse Streaming
to stream the desktop back to the host by launching a program such as notepad
in the guest, and then tabbing to desktop. While this works Steam is designed
to stream this over a local area network and as such employs technologies such
as compression and packetization for network transmission, there are several 
issues with this solution:

  * Latency from guest to client is generally no less then 50-80ms, this is
    very noticable when using a cursor input or running games that demand high
    precision and fast input.

  * Compressing the stream adds additional CPU/GPU overhead that can degrade
    performance.

  * Compressing converts to the YUV422P colorspace which degrades quality
    quite substantially.

  * The stream is tuned for transmission over a network which not only adds
    additional overheads, it also adds additional buffers that increase
    latency.

Our Solution
------------

Qemu contains a virtual device that allows mapping of shared memory between
host and guest called 'ivshmem'. This device has been used for very specific
cases where extremely high bandwidth low latency networking/firewalls have
been required and as such a windows driver was never written for this device.

We developed a Windows driver for this device and have worked with Red Hat to
have it included into the official [VirtIO windows driver repository][1].

The shared memory segment is intialized by the guest with a strcture that
describes the format of the frame and the current mouse coordinates. Frame data
is appended to the buffer after this header.

### Video Transfer

The guest runs a program that uses the ivshmem device as a direct transfer
between the guest and host. This application uses accelerated capture methods
such as NvFBC or DXGI to capture the guests framebuffer. This captured frame is
then copied into the shared memory region. After the frame has been copied a
doorbell is set in ivshmem which the host client application is waiting on,
after which the client application waits for an IRQ from the host to state
that the frame has been processed.

After the host client application is notified there is a new frame it maps the
frame to the host video card by means of a SDL streaming texture. Once a copy
of the frame has been completed the host client then notifies the guest by means
of raising the IRQ the guest is waiting on.

At this time the guest still requires a monitor or dummy device attached that
presents valid EDID information for the resolutions that are desired. There
may be a way around this but that is not the goal of this project at this time
and is thus out of scope. This author found it easiest to just use the
secondary input to one of his monitors to present the EDID information.

### Keyboard and Mouse Input

The host client implements the Spice protocol and is able to send mouse and
keyboard events over the existing communications channels provided by qemu.

If using libvirt the virtual tablet device must be removed from the host as
this application expects to and requires to use relative positioning for all
interactions. If the tablet device exists qemu will give priority to it for
click events which are unreliable when used in this way.

There are also some issues with the qemu PS2 controller that will cause mouse
input lockups when keyboard events and mouse events occur at the same time.
The virtual USB keyboard and mouse inputs while are somewhat more reliable also
suffer random dropouts at high rates due to USB hub emulation timing issues.

At the time of writing this we have found the best solution is to use the PS2
mouse and the VirtIO Keyboard device.

The guest program returns the current mouse position as part of each frame,
this is so that it is possible to syncronise the host mouse position in the
window to that reported by the guest.

We recommend that the guest have the [mouse acceleration disabled][2] so that
the guest's mouse position is 1:1 with the host. This makes for a seemless
experience when the cursor is moved inside and outside of the client
application on the host.

### Audio

This client doesn't implement any form of audio, the guest needs to be
configured to use the hosts for audio directly. At this time we recommend using
pulseaudio as the ALSA implementation seems to be problematic and requires
some attention.

Testing
-------

Our test scenario involved running a physical monitor along side the guest
client, at this time we have only tested NvFBC as the capture technology on the
guest.

### Hardware

|           |                                                            |
|-----------|------------------------------------------------------------|
| MB        | Asrock AB350 Pro4                                          |
| CPU       | AMD Ryzen 7 1700 @ 3.8GHz                                  |
| RAM       | 16GB DDR4                                                  |
| Host GPU  | NVidia GTX 1080 Ti running 4 monitors @ 1920x1200          |
| Guest GPU | NVidia GTX 680 [modified to a Quadro K5000][3] @ 1920x1200 |

### Software

|       |                                                    |
|-------|----------------------------------------------------|
| Host  | Debian 9 + [NPT patch][4]                          |
| Guest | Windows 10 running in KVM on the i440 device tree. |

Results
-------

Latency has not yet been measured, but in practice to this author it is
extremely hard to notice any latency between the physical display and host
client output when streaming 24bit RGB. When streaming YUV422P there no
percevable latency at all, this indicates that the memory copy is the slowest
part of the process. To combat this multi-threaded memory copy has been
implemented on both the host and guest ends of the transfer.

At this time we are waiting for a signed ivshmem driver by Red Hat to be
released but since this may take quite some time we are currently in the
process of obtaining a driver signing certificate.

Status
------

The host software is considered 90% complete for devices that are capable of
NvFBC and has been written in such as way to allow for other capture interfaces
to be easily added to support other platforms.

The client software is considered 90% complete, features such as copy & paste
between client and host are missing and it would be nice if it could also
serve as the ivshmem-server. The client is able to support a variaty of pixel
formats types depending on the capability and performance of the capture API
that is in use.

Security
--------

This project has not been written with security of the shared memory in mind,
it is certainly possible at this time for the client to abuse the headers and
input invalid information that could be used to compromise the host system.

At this time this is not a concern as our use case of this application at this
time is in a completely trusted environment, but before it is more widely put
into use this will need to be addressed.

Improvements
------------

It may be possible to avoid the additional copy by handing the guest a block
of memory directly from the hosts graphics device. This would allow the guest
to stream directly into the texture on the host. This however could be another
avenue of escape for malicious code on the guest and would need very careful
consideration before any attempt is made to implement this.

To achieve the above it would be required to modify qemu integrating the
features of this client directly so that a SDL\_Texture's RAM could be mapped
directly into the guest.

License
-------

~~~
KVMGFX Client - A KVM Client for VGA Passthrough
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
~~~

[1]: https://github.com/virtio-win/kvm-guest-drivers-windows/tree/master/ivshmem
[2]: http://donewmouseaccel.blogspot.com.au/2010/03/markc-windows-7-mouse-acceleration-fix.html
[3]: http://www.eevblog.com/forum/chat/hacking-nvidia-cards-into-their-professional-counterparts/
[4]: https://patchwork.kernel.org/patch/10027525/
