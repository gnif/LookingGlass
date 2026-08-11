.. _client_options:
.. _client_cli_options:
.. _client_full_command_options:

Client option reference
#######################

The exact option set depends on how the client was built. Use this command for
the complete reference, including defaults and short forms:

.. code:: bash

   looking-glass-client --help

Use only the canonical names shown by that command. Older configuration names
may remain as compatibility aliases, but they print a warning and can be
removed in a later release.

Syntax
------

Long options use ``section:name=value``:

.. code:: bash

   looking-glass-client win:fullScreen=yes lgmp:allowDMA=no

Short options may take a separate argument:

.. code:: bash

   looking-glass-client -F -f /dev/kvmfr1

Configuration files use the section as an INI heading:

.. code-block:: ini

   [win]
   fullScreen=yes

   [lgmp]
   allowDMA=no

Common options
--------------

.. list-table:: Application and transport
   :widths: 34 16 50
   :header-rows: 1

   * - Option
     - Default
     - Purpose
   * - ``app:transport``
     - ``lgmp``
     - Select the primary transport, normally ``lgmp`` or ``spice``
   * - ``lgmp:shmDevice``
     - automatic
     - Select the KVMFR device or shared-memory file
   * - ``lgmp:allowDMA``
     - ``yes``
     - Permit direct GPU imports when supported
   * - ``spice:enable``
     - ``yes``
     - Enable the built-in SPICE transport and fallback services
   * - ``spice:host``
     - ``127.0.0.1``
     - Set the SPICE server address or Unix socket
   * - ``spice:port``
     - ``5900``
     - Set the SPICE port; 0 selects a Unix socket
   * - ``spice:input``
     - ``yes``
     - Permit SPICE input fallback
   * - ``spice:clipboard``
     - ``yes``
     - Permit SPICE clipboard service
   * - ``spice:audio``
     - ``yes``
     - Permit SPICE audio service
   * - ``spice:usbAudio``
     - ``yes``
     - Use the recommended emulated USB audio device instead of classic SPICE
       audio

.. list-table:: Window and display
   :widths: 34 16 50
   :header-rows: 1

   * - Option
     - Default
     - Purpose
   * - ``win:size``
     - ``1024x768``
     - Set the initial client window size
   * - ``win:fullScreen``
     - ``no``
     - Start full screen
   * - ``win:autoResize``
     - ``no``
     - Follow guest resolution changes
   * - ``win:setGuestRes``
     - ``yes``
     - Ask a supporting producer to follow the client viewport
   * - ``win:fpsMin``
     - automatic
     - Set the minimum redraw rate; 0 disables it and is not recommended
   * - ``win:jitRender``
     - ``no``
     - Render close to the predicted presentation deadline
   * - ``win:showFPS``
     - ``no``
     - Show the FPS and UPS widget
   * - ``egl:mapHDRtoSDR``
     - ``yes``
     - Tone-map HDR frames for an SDR output
   * - ``egl:preset``
     - none
     - Load a named filter preset at startup

.. list-table:: Input
   :widths: 34 16 50
   :header-rows: 1

   * - Option
     - Default
     - Purpose
   * - ``input:escapeKey``
     - ``KEY_SCROLLLOCK``
     - Set the capture/menu key; use ``help`` to list accepted names
   * - ``input:autoCapture``
     - ``no``
     - Grab the keyboard inside the guest view and release it before exit
   * - ``input:captureOnly``
     - ``no``
     - Enable guest input only while captured
   * - ``input:grabKeyboard``
     - ``yes``
     - Grab the keyboard in capture mode
   * - ``input:rawMouse``
     - ``no``
     - Use raw relative movement in capture mode
   * - ``input:mouseRedraw``
     - ``yes``
     - Repaint at display cadence when only the cursor changes

.. list-table:: Audio and clipboard
   :widths: 34 16 50
   :header-rows: 1

   * - Option
     - Default
     - Purpose
   * - ``clipboard:toVM``
     - ``yes``
     - Allow clipboard transfers to the guest
   * - ``clipboard:toLocal``
     - ``yes``
     - Allow clipboard transfers from the guest
   * - ``audio:periodSize``
     - ``512``
     - Request an audio backend period in samples
   * - ``audio:latencyOffset``
     - ``6``
     - Add safety margin to the classic SPICE audio buffer in milliseconds
   * - ``audio:resampler``
     - ``auto``
     - Select classic SPICE resampling with ``auto``, ``libsamplerate`` or the
       backend
   * - ``audio:micDefault``
     - ``prompt``
     - Select ``prompt``, ``allow`` or ``deny`` for microphone requests
   * - ``audio:debug``
     - ``no``
     - Log detailed audio synchronization statistics

Advanced options
----------------

Polling intervals, renderer damage handling, display-server protocol choices
and audio-device selectors are intentionally omitted here. They are useful for
diagnosis but can make latency or reliability worse when changed without a
specific reason. Consult ``--help`` and record the original value before
experimenting.
