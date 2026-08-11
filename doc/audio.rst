.. _client_audio:

Audio and microphone
####################

The client selects audio independently from video and input. The current LGMP
transport does not provide audio, so SPICE normally supplies it in an IDD
setup. Selecting SPICE as the primary video transport also uses its audio
provider.

.. warning::

   Do not use Scream for a Looking Glass setup. Use the built-in classic SPICE
   or USB audio path. Scream over IVSHMEM can also conflict with the shared
   device used for Looking Glass frames.

Looking Glass offers two SPICE audio paths:

USB audio (recommended)
   Presents an emulated Looking Glass USB Audio Class 2 device to Windows over
   a SPICE USB channel. It supports stereo, quadraphonic, 5.1 and 7.1 playback
   using 24-bit PCM at up to 192 kHz. It also provides asynchronous playback
   feedback and a lower-latency clock relationship with supported Linux audio
   backends. This path is enabled by default when it is available.

Classic SPICE audio
   Uses a virtual guest sound card and SPICE playback and record channels.
   QEMU limits it to stereo, 16-bit samples at 48 kHz. Use it as a compatibility
   fallback when the emulated USB audio device is not available.

Set ``spice:audio=no`` to disable both paths.

.. _client_usb_audio:

USB audio setup
---------------

USB audio requires:

* a client built with ``libusbredirparser-0.5`` version 0.7.1 or newer;
* at least one unused SPICE USB redirection channel in the VM; and
* a working client playback backend.

For libvirt, add a USB redirection device:

.. code:: xml

   <redirdev bus='usb' type='spicevmc'/>

In virt-manager, this is a **USB Redirector** device using the SPICE channel.
It is dedicated to the virtual Looking Glass audio device while USB audio is
active.

The emulated USB audio device is enabled by default. Its settings are:

.. code-block:: ini

   [spice]
   audio=yes
   usbAudio=yes

Playback supports stereo, quadraphonic, 5.1 and 7.1 speaker layouts using
24-bit PCM at sample rates up to 192 kHz. Recording supports stereo at up to
192 kHz.

After the device connects, select the Looking Glass USB Audio speakers as the
Windows output device.

If USB audio could not be created at startup, the client falls back to classic
SPICE audio and logs the reason. A working USB audio device also requires the
SPICE USB redirection channel to connect; if the channel is absent, add it to
the VM rather than reinstalling the Windows endpoint.

Classic SPICE setup
-------------------

Add an Intel HDA sound device and a SPICE audio backend to the VM as shown in
:ref:`libvirt_spice_server`, then select the classic path:

.. code-block:: ini

   [spice]
   audio=yes
   usbAudio=no

QEMU's classic SPICE audio implementation exposes a fixed stereo format: two
channels, 16-bit samples at 48 kHz. Use the emulated USB audio device when
other sample rates, sample formats or channel layouts are required.

Microphone access
-----------------

PipeWire supports playback and recording. The current PulseAudio backend does
not provide microphone recording. Press :kbd:`ScrLk` + :kbd:`E` to toggle
recording when it is available.

When a guest application opens the microphone, ``audio:micDefault`` controls
the response:

``prompt``
   Ask before sending microphone audio. This is the default.

``allow``
   Allow requests automatically.

``deny``
   Refuse requests automatically.

Press :kbd:`ScrLk` + :kbd:`C` to cycle this policy. Keep
``audio:micShowIndicator=yes`` to show when recording is active.

Latency settings
----------------

``audio:periodSize`` requests the audio backend period in samples. The default
is 512; 0 requests a 10 ms period. Smaller values may reduce latency but also
make underruns more likely.

``audio:latencyOffset`` applies only to classic SPICE audio. It adds safety
margin to the calculated minimum buffer; the default is 6 ms. Reduce it only
after confirming that classic SPICE playback remains free of dropouts under
load.

``audio:resampler`` also applies only to classic SPICE audio. The default
``auto`` setting chooses the appropriate path; the other choices are
``libsamplerate`` and ``backend``. The emulated USB audio device instead uses
feedback to adjust the Windows packet rate.

Set ``audio:debug=yes`` to log ring-buffer level, backend delay, clock feedback
and underrun or overrun counts. Disable it after diagnosis to keep the normal
log concise.
