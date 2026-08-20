.. _client_usage:

Use the client
##############

Start the client after the Windows guest and IDD are running:

.. code:: bash

   looking-glass-client

The client uses LGMP over ``/dev/kvmfr0`` automatically when that device is
available. Otherwise it uses ``/dev/shm/looking-glass``. Select another device
with ``-f`` or the canonical ``lgmp:shmDevice`` option:

.. code:: bash

   looking-glass-client -f /dev/kvmfr1

Use ``looking-glass-client --help`` to see the options supported by the
installed build. Command-line options override configuration files.

.. _client_key_bindings:

Default key bindings
--------------------

Looking Glass uses :kbd:`ScrLk` as its escape key by default. Press it by
itself to enter or leave capture mode. Hold it to display the available
commands. Change it with ``input:escapeKey`` or ``-m`` if the keyboard does
not have :kbd:`ScrLk`.

.. list-table:: Default client commands
   :widths: 35 65
   :header-rows: 1

   * - Command
     - Action
   * - :kbd:`ScrLk`
     - Enter or leave capture mode
   * - :kbd:`ScrLk` + :kbd:`Q`
     - Quit
   * - :kbd:`ScrLk` + :kbd:`F`
     - Toggle full screen
   * - :kbd:`ScrLk` + :kbd:`V`
     - Toggle the video stream
   * - :kbd:`ScrLk` + :kbd:`R`
     - Rotate clockwise by 90 degrees
   * - :kbd:`ScrLk` + :kbd:`Shift` + :kbd:`R`
     - Force guest display recovery mode
   * - :kbd:`ScrLk` + :kbd:`=`
     - Ask the IDD to match the client window resolution
   * - :kbd:`ScrLk` + :kbd:`I`
     - Toggle guest input
   * - :kbd:`ScrLk` + :kbd:`O`
     - Enter or leave interactive overlay mode
   * - :kbd:`ScrLk` + :kbd:`D`
     - Toggle the FPS and UPS widget
   * - :kbd:`ScrLk` + :kbd:`T`
     - Toggle the frame-timing graphs
   * - :kbd:`ScrLk` + :kbd:`N`
     - Toggle EGL night vision
   * - :kbd:`ScrLk` + :kbd:`E`
     - Toggle microphone recording when the audio backend supports it
   * - :kbd:`ScrLk` + :kbd:`C`
     - Cycle the default microphone permission
   * - :kbd:`ScrLk` + :kbd:`M`
     - Send mute to the guest
   * - :kbd:`ScrLk` + :kbd:`Up` or :kbd:`Down`
     - Send volume up or down to the guest
   * - :kbd:`ScrLk` + :kbd:`Insert` or :kbd:`Delete`
     - Adjust capture-mode mouse sensitivity
   * - :kbd:`ScrLk` + :kbd:`LWin` or :kbd:`RWin`
     - Send that Windows key to the guest
   * - :kbd:`ScrLk` + :kbd:`F1` through :kbd:`F12`
     - Send :kbd:`Ctrl` + :kbd:`Alt` + that function key to a Linux guest

The microphone commands are registered only when the client was built with
audio recording support. Night vision is an EGL feature. The virtual-console
bindings are registered only when the selected guest type is Linux.

.. _client_config_options_file:

Configuration files
-------------------

The client loads these files in order when they exist:

* ``/etc/looking-glass-client.ini``
* ``~/.looking-glass-client.ini``
* ``$XDG_CONFIG_HOME/looking-glass/client.ini``

Later files override earlier files. The usual per-user path is
``~/.config/looking-glass/client.ini``. Files use INI syntax:

.. code-block:: ini

   [win]
   fullScreen=yes
   setGuestRes=yes

   [input]
   autoCapture=yes

   [spice]
   clipboard=yes
   audio=yes

   [egl]
   preset=my-preset

Boolean values accept ``yes`` or ``no``. Long command-line options use
``section:name=value``, for example:

.. code:: bash

   looking-glass-client win:fullScreen=yes input:autoCapture=yes

.. _client_overlay_mode:
.. _client_config_widget:

Interactive overlay
-------------------

Press :kbd:`ScrLk` + :kbd:`O` to make the overlay interactive. Press
:kbd:`Esc` or the same binding to leave it. The overlay can:

* enable and arrange the FPS and timing widgets;
* select and configure EGL filters;
* save filter settings as presets; and
* enable diagnostic views such as damage rectangles.

Widget positions and sizes are stored in
``$XDG_CONFIG_HOME/looking-glass/imgui.ini``. EGL filter presets are stored in
``$XDG_CONFIG_HOME/looking-glass/presets``. Do not edit either while the
client is running.

Runtime changes are not all written to the main client configuration. Put
options that must apply at every start in ``client.ini``.

User guides
-----------

.. toctree::
   :maxdepth: 1

   input
   display
   audio
   performance
   options
