.. _client_usage:

Client usage
------------

.. raw:: html

   <p><code class="literal"><b>looking-glass-client</b> [--help] [-f] [-F] [-s] [-S] [options...]</code></p>


.. _client_cli_options:

Command line options
~~~~~~~~~~~~~~~~~~~~

A full list of command line options is available with the ``--help`` or ``-h``
options.

Example: ``looking-glass-client --help``

Common options are listed below:

================  ===========================================
 Short option      Description
================  ===========================================
 ``-f shmFile``    use ``shmFile`` for IVSHMEM shared memory
 ``-F``            automatically enter full screen
 ``-s``            disable spice
 ``-S``            disable host screensaver
================  ===========================================

Options may be provided in short form when available, or long form.
Boolean options may be specified without a parameter to toggle their
state.

Examples:

- ``looking-glass-client -F`` (short)
- ``looking-glass-client win:fullScreen`` (long)
- ``looking-glass-client -f /dev/shm/my-lg-shmem`` (short with parameter)
- ``looking-glass-client app:shmFile=/dev/shm/my-lg-shmem`` (long with parameter)

.. seealso::

   :ref:`client_full_command_options`

.. _client_key_bindings:

Default key bindings
~~~~~~~~~~~~~~~~~~~~

By default, Looking Glass uses the :kbd:`ScrLk` key as the escape key
for commands, as well as the input :kbd:`capture` mode toggle; this can be
changed using the ``-m`` switch if you desire a different key. Below are
a list of current key bindings:

============================ =======================================================
Command                      Description
============================ =======================================================
:kbd:`ScrLk`                 Toggle capture mode
:kbd:`ScrLk` + :kbd:`Q`      Quit
:kbd:`ScrLk` + :kbd:`E`      Toggle audio recording
:kbd:`ScrLk` + :kbd:`R`      Rotate the output clockwise by 90° increments
:kbd:`ScrLk` + :kbd:`T`      Show frame timing information
:kbd:`ScrLk` + :kbd:`I`      Spice keyboard & mouse enable toggle
:kbd:`ScrLk` + :kbd:`O`      Toggle overlay
:kbd:`ScrLk` + :kbd:`D`      FPS display toggle
:kbd:`ScrLk` + :kbd:`F`      Full screen toggle
:kbd:`ScrLk` + :kbd:`V`      Video stream toggle
:kbd:`ScrLk` + :kbd:`N`      Toggle night vision mode
:kbd:`ScrLk` + :kbd:`F1`     Send :kbd:`Ctrl` + :kbd:`Alt` + :kbd:`F1` to the guest
:kbd:`ScrLk` + :kbd:`F2`     Send :kbd:`Ctrl` + :kbd:`Alt` + :kbd:`F2` to the guest
:kbd:`ScrLk` + :kbd:`F3`     Send :kbd:`Ctrl` + :kbd:`Alt` + :kbd:`F3` to the guest
:kbd:`ScrLk` + :kbd:`F4`     Send :kbd:`Ctrl` + :kbd:`Alt` + :kbd:`F4` to the guest
:kbd:`ScrLk` + :kbd:`F5`     Send :kbd:`Ctrl` + :kbd:`Alt` + :kbd:`F5` to the guest
:kbd:`ScrLk` + :kbd:`F6`     Send :kbd:`Ctrl` + :kbd:`Alt` + :kbd:`F6` to the guest
:kbd:`ScrLk` + :kbd:`F7`     Send :kbd:`Ctrl` + :kbd:`Alt` + :kbd:`F7` to the guest
:kbd:`ScrLk` + :kbd:`F8`     Send :kbd:`Ctrl` + :kbd:`Alt` + :kbd:`F8` to the guest
:kbd:`ScrLk` + :kbd:`F9`     Send :kbd:`Ctrl` + :kbd:`Alt` + :kbd:`F9` to the guest
:kbd:`ScrLk` + :kbd:`F10`    Send :kbd:`Ctrl` + :kbd:`Alt` + :kbd:`F10` to the guest
:kbd:`ScrLk` + :kbd:`F11`    Send :kbd:`Ctrl` + :kbd:`Alt` + :kbd:`F11` to the guest
:kbd:`ScrLk` + :kbd:`F12`    Send :kbd:`Ctrl` + :kbd:`Alt` + :kbd:`F12` to the guest
:kbd:`ScrLk` + :kbd:`M`      Send mute to the guest
:kbd:`ScrLk` + :kbd:`↑`      Send volume up to the guest
:kbd:`ScrLk` + :kbd:`↓`      Send volume down to the guest
:kbd:`ScrLk` + :kbd:`Insert` Increase mouse sensitivity in capture mode
:kbd:`ScrLk` + :kbd:`Del`    Decrease mouse sensitivity in capture mode
:kbd:`ScrLk` + :kbd:`LWin`   Send :kbd:`LWin` to the guest
:kbd:`ScrLk` + :kbd:`RWin`   Send :kbd:`RWin` to the guest
============================ =======================================================

You can also find this list at any time by holding down :kbd:`ScrLk`.

.. _client_config_options_file:

Configuration files
~~~~~~~~~~~~~~~~~~~

By default, Looking Glass will load config files from
the following locations:

-  ``/etc/looking-glass-client.ini``
-  ``~/.looking-glass-client.ini``
-  ``$XDG_CONFIG_HOME/looking-glass/client.ini`` (usually ``~/.config/looking-glass/client.ini``)

All config files are loaded in order. Duplicate entries override earlier ones.
This means you can set a system-wide configuration in
``/etc/looking-glass-client.ini``, and override specific options for just
your user in ``~/.looking-glass-client.ini``, which is overlayed on top of
the system-wide configuration.

When first launched, the Looking-Glass client will create the folder
``$XDG_CONFIG_HOME/looking-glass/`` if it does not yet exist.

The format of config files is the commonly known INI format, for example:

.. code-block:: ini

   [win]
   fullScreen=yes

   [egl]
   nvGain=1

   ; this is a comment

Command line arguments will override any options loaded from config
files.

.. _client_overlay_mode:

Overlay mode
~~~~~~~~~~~~

The Overlay Mode lets you configure various runtime options for Looking Glass.
These include:

- EGL filters
- Performance metrics options
- Debug frame damage display

(see :ref:`client_config_widget`)

You can also reposition and resize enabled widgets, like the FPS/UPS display,
and performance metrics.

Enter and exit Overlay Mode with :kbd:`ScrLk` + :kbd:`O`.
:kbd:`ESC` can also be used to exit. (see :ref:`client_key_bindings`)

Modifications done to widgets in overlay mode are stored in
``$XDG_CONFIG_HOME/looking-glass/imgui.ini``.
Please do not manually edit this file while Looking Glass is running,
as your changes may be discarded.

.. _client_config_widget:

Configuration widget
~~~~~~~~~~~~~~~~~~~~

The configuration widget is accessible through the overlay mode. The
widget has multiple tabs that allow setting a variety of modes and
parameters for Looking Glass at runtime.

Settings tab
^^^^^^^^^^^^

- *Performance Metrics*: A toggle for the performance metrics widget.
  Multiple graphs are available, and they will stack vertically.
- *EGL*: Modify EGL settings, such as the algorithm used for scaling, and
  night vision mode.

Changes in the settings tab are not persistent, and will be reset back to
their default values when the client is restarted.

EGL filters tab
^^^^^^^^^^^^^^^

The EGL filters tab contains options for toggling, configuring, and ordering
post-processing filters. Each filter can be expanded to open its settings.
Filters can also be re-ordered by dragging them up or down. Filters are applied
from top to bottom. Keep this in mind when ordering them -- for example,
applying CAS before FSR might have different results than the reverse. Users
are encouraged to experiment with the order and parameters to achieve optimal
results. The currently available filters include:

-  *Downscaler*: Filter for downscaling the host resolution. Can be used to undo
   poor upscaling on the VM to better utilize AMD FSR (see below). The filter
   has a pixel-size setting that is used to set the effective downscaling ratio,
   and a configurable interpolation algorithm.

-  *AMD FidelityFX Super Resolution (FSR)*: Spatial upscaling filter that works
   on low resolution frames from the guest VM and intelligently upscales to a
   higher resolution. The filter sharpness is tunable, and displays the
   equivalent AMD quality mode based on the resolution difference.

-  *AMD FidelityFX Contrast Adaptive Sharpening (CAS)*: Filter that
   increases visual quality by applying a sharpening algorithm to the
   video. CAS can sometimes restore detail lost in a typical upscaling
   application. Has adjustable sharpness setting.

The filter settings and order can be saved to presets so that it can be restored
at a later time. As filter settings are usually application specific, multiple
presets can be defined for each case scenario. To save a preset, click on *"Save
preset as..."* and enter a preset name. Presets are loaded by selecting them in
the *Preset name* pull down. Presets are persistent and are stored on disk at
``$XDG_CONFIG_HOME/looking-glass/presets``.

.. warning::
   Please refrain from modifying any files under the ``presets`` folder.
   Those files are meant to be modified only by the Looking-Glass client.

.. note::
   Although presets are persistent, the client will not remember which
   preset was used last session, so a preset needs to be recalled once
   the client starts.

.. _client_full_command_options:

All command line options
~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block::

   +------------------------+-------+------------------------+-----------------------------------------------------------------------------------------+
   | Long                   | Short | Value                  | Description                                                                             |
   +------------------------+-------+------------------------+-----------------------------------------------------------------------------------------+
   | app:configFile         | -C    | NULL                   | A file to read additional configuration from                                            |
   | app:renderer           | -g    | auto                   | Specify the renderer to use                                                             |
   | app:license            | -l    | no                     | Show the license for this application and then terminate                                |
   | app:cursorPollInterval |       | 1000                   | How often to check for a cursor update in microseconds                                  |
   | app:framePollInterval  |       | 1000                   | How often to check for a frame update in microseconds                                   |
   | app:allowDMA           |       | yes                    | Allow direct DMA transfers if supported (see `README.md` in the `module` dir)           |
   | app:shmFile            | -f    | /dev/shm/looking-glass | The path to the shared memory file, or the name of the kvmfr device to use, e.g. kvmfr0 |
   +------------------------+-------+------------------------+-----------------------------------------------------------------------------------------+

   +-------------------------+-------+------------------------+----------------------------------------------------------------------+
   | Long                    | Short | Value                  | Description                                                          |
   +-------------------------+-------+------------------------+----------------------------------------------------------------------+
   | win:title               |       | Looking Glass (client) | The window title                                                     |
   | win:position            |       | center                 | Initial window position at startup                                   |
   | win:size                |       | 1024x768               | Initial window size at startup                                       |
   | win:autoResize          | -a    | no                     | Auto resize the window to the guest                                  |
   | win:allowResize         | -n    | yes                    | Allow the window to be manually resized                              |
   | win:keepAspect          | -r    | yes                    | Maintain the correct aspect ratio                                    |
   | win:forceAspect         |       | yes                    | Force the window to maintain the aspect ratio                        |
   | win:dontUpscale         |       | no                     | Never try to upscale the window                                      |
   | win:intUpscale          |       | no                     | Allow only integer upscaling                                         |
   | win:shrinkOnUpscale     |       | no                     | Limit the window dimensions when dontUpscale is enabled              |
   | win:borderless          | -d    | no                     | Borderless mode                                                      |
   | win:fullScreen          | -F    | no                     | Launch in fullscreen borderless mode                                 |
   | win:maximize            | -T    | no                     | Launch window maximized                                              |
   | win:minimizeOnFocusLoss |       | no                     | Minimize window on focus loss                                        |
   | win:fpsMin              | -K    | -1                     | Frame rate minimum (0 = disable - not recommended, -1 = auto detect) |
   | win:ignoreQuit          | -Q    | no                     | Ignore requests to quit (i.e. Alt+F4)                                |
   | win:noScreensaver       | -S    | no                     | Prevent the screensaver from starting                                |
   | win:autoScreensaver     |       | no                     | Prevent the screensaver from starting when guest requests it         |
   | win:alerts              | -q    | yes                    | Show on screen alert messages                                        |
   | win:quickSplash         |       | no                     | Skip fading out the splash screen when a connection is established   |
   | win:overlayDimsDesktop  |       | yes                    | Dim the desktop when in interactive overlay mode                     |
   | win:rotate              |       | 0                      | Rotate the displayed image (0, 90, 180, 270)                         |
   | win:uiFont              |       | DejaVu Sans Mono       | The font to use when rendering on-screen UI                           |
   | win:uiSize              |       | 14                     | The font size to use when rendering on-screen UI                     |
   | win:jitRender           |       | no                     | Enable just-in-time rendering                                        |
   | win:showFPS             | -k    | no                     | Enable the FPS & UPS display                                         |
   +-------------------------+-------+------------------------+----------------------------------------------------------------------+

   +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
   | Long                         | Short | Value               | Description                                                                      |
   +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
   | input:grabKeyboard           | -G    | yes                 | Grab the keyboard in capture mode                                                |
   | input:grabKeyboardOnFocus    |       | no                  | Grab the keyboard when focused                                                   |
   | input:releaseKeysOnFocusLoss |       | yes                 | On focus loss, send key up events to guest for all held keys                     |
   | input:escapeKey              | -m    | 70 = KEY_SCROLLLOCK | Specify the escape/menu key to use (use "help" to see valid values)              |
   | input:ignoreWindowsKeys      |       | no                  | Do not pass events for the windows keys to the guest                             |
   | input:hideCursor             | -M    | yes                 | Hide the local mouse cursor                                                      |
   | input:mouseSens              |       | 0                   | Initial mouse sensitivity when in capture mode (-9 to 9)                         |
   | input:mouseSmoothing         |       | yes                 | Apply simple mouse smoothing when rawMouse is not in use (helps reduce aliasing) |
   | input:rawMouse               |       | no                  | Use RAW mouse input when in capture mode (good for gaming)                       |
   | input:mouseRedraw            |       | yes                 | Mouse movements trigger redraws (ignores FPS minimum)                            |
   | input:autoCapture            |       | no                  | Try to keep the mouse captured when needed                                       |
   | input:captureOnly            |       | no                  | Only enable input via SPICE if in capture mode                                   |
   | input:helpMenuDelay          |       | 200                 | Show help menu after holding down the escape key for this many milliseconds      |
   +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+

   +------------------------+-------+-----------+---------------------------------------------------------------------+
   | Long                   | Short | Value     | Description                                                         |
   +------------------------+-------+-----------+---------------------------------------------------------------------+
   | spice:enable           | -s    | yes       | Enable the built in SPICE client for input and/or clipboard support |
   | spice:host             | -c    | 127.0.0.1 | The SPICE server host or UNIX socket                                |
   | spice:port             | -p    | 5900      | The SPICE server port (0 = unix socket)                             |
   | spice:input            |       | yes       | Use SPICE to send keyboard and mouse input events to the guest      |
   | spice:clipboard        |       | yes       | Use SPICE to synchronize the clipboard contents with the guest      |
   | spice:clipboardToVM    |       | yes       | Allow the clipboard to be synchronized TO the VM                    |
   | spice:clipboardToLocal |       | yes       | Allow the clipboard to be synchronized FROM the VM                  |
   | spice:audio            |       | yes       | Enable SPICE audio support                                          |
   | spice:scaleCursor      | -j    | yes       | Scale cursor input position to screen size when up/down scaled      |
   | spice:captureOnStart   |       | no        | Capture mouse and keyboard on start                                 |
   | spice:alwaysShowCursor |       | no        | Always show host cursor                                             |
   | spice:showCursorDot    |       | yes       | Use a "dot" cursor when the window does not have focus              |
   +------------------------+-------+-----------+---------------------------------------------------------------------+

   +------------------------+-------+--------+-------------------------------------------------------------------------------+
   | Long                   | Short | Value  | Description                                                                   |
   +------------------------+-------+--------+-------------------------------------------------------------------------------+
   | audio:periodSize       |       | 2048   | Requested audio device period size in samples                                 |
   | audio:bufferLatency    |       | 13     | Additional buffer latency in milliseconds                                     |
   | audio:micDefault       |       | prompt | Default action when an application opens the microphone (prompt, allow, deny) |
   | audio:micShowIndicator |       | yes    | Display microphone usage indicator                                            |
   +------------------------+-------+--------+-------------------------------------------------------------------------------+

   +------------------+-------+-------+---------------------------------------------------------------------------+
   | Long             | Short | Value | Description                                                               |
   +------------------+-------+-------+---------------------------------------------------------------------------+
   | egl:vsync        |       | no    | Enable vsync                                                              |
   | egl:doubleBuffer |       | no    | Enable double buffering                                                   |
   | egl:multisample  |       | yes   | Enable Multisampling                                                      |
   | egl:nvGainMax    |       | 1     | The maximum night vision gain                                             |
   | egl:nvGain       |       | 0     | The initial night vision gain at startup                                  |
   | egl:cbMode       |       | 0     | Color Blind Mode (0 = Off, 1 = Protanope, 2 = Deuteranope, 3 = Tritanope) |
   | egl:scale        |       | 0     | Set the scale algorithm (0 = auto, 1 = nearest, 2 = linear)               |
   | egl:debug        |       | no    | Enable debug output                                                       |
   | egl:noBufferAge  |       | no    | Disable partial rendering based on buffer age                             |
   | egl:noSwapDamage |       | no    | Disable swapping with damage                                              |
   | egl:scalePointer |       | yes   | Keep the pointer size 1:1 when downscaling                                |
   | egl:preset       |       | NULL  | The initial filter preset to load                                         |
   +------------------+-------+-------+---------------------------------------------------------------------------+

   +----------------------+-------+-------+---------------------------------------------+
   | Long                 | Short | Value | Description                                 |
   +----------------------+-------+-------+---------------------------------------------+
   | opengl:mipmap        |       | yes   | Enable mipmapping                           |
   | opengl:vsync         |       | no    | Enable vsync                                |
   | opengl:preventBuffer |       | yes   | Prevent the driver from buffering frames    |
   | opengl:amdPinnedMem  |       | yes   | Use GL_AMD_pinned_memory if it is available |
   +----------------------+-------+-------+---------------------------------------------+

   +-----------------------+-------+-------+-------------------------+
   | Long                  | Short | Value | Description             |
   +-----------------------+-------+-------+-------------------------+
   | wayland:warpSupport   |       | yes   | Enable cursor warping   |
   | wayland:fractionScale |       | yes   | Enable fractional scale |
   +-----------------------+-------+-------+-------------------------+

.. _host_usage:

Host usage
----------

By default the host application will simply work however there are some
configurable options available. While the host application will accept command
line arguments just as the client will it is more convenient to create the
``looking-glass-host.ini`` file with the desired configuration options.

This file must be placed in the same directory that the Looking Glass host
application was installed for it to be found and used by the application

.. _host_capture:

Capture interface
~~~~~~~~~~~~~~~~~

.. note::
  Currently we only provide support for the Windows host application, Linux
  options are not currently documented.

Currently under windows there are two capture interfaces available for use,
by default the most compatible and commonly supported interface is selected
however this can be changed via the ini file with the following configuration:

.. code:: ini

 [app]
 capture=<INTERFACE>

Where ``<INTERFACE>`` is one of ``dxgi`` or ``nvfbc``

.. _host_capture_dxgi:

Microsoft DXGI Desktop Duplication
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

This interface (DXGI) is the default and most compatible capture interface for
windows, unfortunately though it does suffer from several drawbacks over other
options. DXGI capture can operate in two modes, DirectX 11 (default) or the
experimental and unofficial DirectX 12 mode.

Due to the design of Microsoft's DXGI API and the decision made to roll
hardware cursor updates into the capture stream this interface can suffer from
microstutters when the mouse is being moved/updated. This issue only affects
guest applications that make use of the hardware cursor instead of compositing
the cursor directly, as such titles that do not use a mouse (most FPV games)
are not affected.

The other drawback of this API is the overall system overhead, however this can
be mitigated by using the DirectX 12 backend. Please be aware though that this
backend is not experimental because it's new, but rather it's a slight
abuse/misuse of the DXGI API and allows us to bypass some Windows internals.

To enable the DirectX 12 backend the following configuration needs to be added
to the ``looking-glass-host.ini`` configuration:

.. code:: ini

  [app]
  capture=dxgi

  [dxgi]
  copyBackend=d3d12
  d3d12CopySleep=5
  disableDamage=false

The option ``d3d12CopySleep`` is to work around the lack of locking this misuse
of the API allows and you will need to tune this value to what suits your
hardware best. The default value is 5ms as this should work for most, lowing
it below 2ms is doubtful to be of practical use to anyone. If this value is too
low you may see screen corruption which is usually most evident while dragging
a window around on the Windows desktop.

.. note::
   Lowering d3d12CopySleep can improve the UPS however the UPS metric makes
   little sense when using the d3d12 backend as if this value is too low
   unchanged frames will be doubled up.

The ``disableDamage`` option may be needed to avoid screen corruption.  Note
that this will increase the bandwidth required and in turn the overall load on
your system.

The DXGI capture interface also offers a feature that allows downsampling the
captured frames in the guest GPU before transferring them to shared memory.
This feature is very useful if you are super scaling for better picture quality
and wish to reduce system memory pressure.

The configuration for this is fairly straightforward and is defined as set of
rules to determine when to perform this downsampling. The format is as follows:

.. code:: ini

  [dxgi]
  downsample=RULE1,RULE2,RULE3

The rules are written as follows:

.. code::

  (>|>=)(WIDTH)x(HEIGHT):(LEVEL)

The ``LEVEL`` is the fractional scale level where 1 = 50%, 2 = 25%, 3 = 12.5%.

**Examples:**

.. code:: ini

 [dxgi]
 ; Downsample anything greater then 1920x1080 to 50% of it's original size
 downsample=>1920:1080:1

 ; Downsample exactly 1920x1080 to 25% of it's original size, and anything greater
 ; then 1920x1080 to 50% of it's original size.
 downsample=1920x1080:1,>1920x1080:2

 ; Downsample anything greater or equal to 1920x1080 to 50% of it's original size
 downsample=>=1920x1080:1

.. _host_capture_nvfbc:

NVIDIA Frame Buffer Capture
^^^^^^^^^^^^^^^^^^^^^^^^^^^

Due to the NVIDIA SDK License agreement this GPU feature is only available on
professional/workstation GPUs such as the Quadro series. It is known however
that **all** NVIDIA GPUs are capable of this as both GeForce Experience and
Steam are able to make use of it.

If you are able to make use/enable this this feature it offers lower overall
system load and lower latency capture, and does not suffer from the mouse
motion stutter issues that DXGI suffers from.

To enable it's usage use the following configuration in the
``looking-glass-host.ini`` file:

.. code:: ini

  [app]
  capture=nvfbc

If this feature is unavailable to you the host application will fail to start
and the host log will contain an error stating that the feature is not
available.

The NVFBC capture interface also offers a feature much like DXGI to allow
downsampling the captured frames in the guest GPU before transferring them to
shared memory. However unlike DXGI which is limited to fractional scaling,
NvFBC is able to scale to any arbitrary resolution.

The configuration for this is fairly straight forward and is defined as set of
rules to determine when to perform this downsampling. The format is as follows:

.. code:: ini

  [nvfbc]
  downsample=RULE1,RULE2,RULE3

The rules are written as follows:

.. code::

   (>|>=)(WIDTH)x(HEIGHT):(TARGET WIDTH)x(TARGET HEIGHT)

**Examples:**

.. code:: ini

  [nvfbc]
  ; Downsample exactly 3840x2160 to 1920x1080
  downsample=3840x2160:1920x1080

  ; Downsample anything greater then 1920x1080 to 1920x1080
  downsample=>1920x1080:1920x1080

  ; Downsample 3840x2160 to 1920x1080, or 3840x2400 to 1920x1200
  downsample=3840x2160:1920x1080,3840x2400:1920x1200

This capture interface also looks for and reads the value of the system
environment variable ``NVFBC_PRIV_DATA`` if it has been set, documentation on
its usage however is unavailable.
