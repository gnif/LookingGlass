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

  +------------------------+-------+-------------+-----------------------------------------------------------------------------------------+
  | Long                   | Short | Value       | Description                                                                             |
  +========================+=======+=============+=========================================================================================+
  | app:configFile         | -C    | NULL        | A file to read additional configuration from                                            |
  +------------------------+-------+-------------+-----------------------------------------------------------------------------------------+
  | app:renderer           | -g    | EGL         | Specify the renderer to use                                                             |
  +------------------------+-------+-------------+-----------------------------------------------------------------------------------------+
  | app:license            | -l    | no          | Show the license for this application and then terminate                                |
  +------------------------+-------+-------------+-----------------------------------------------------------------------------------------+
  | app:cursorPollInterval |       | 1000        | How often to check for a cursor update in microseconds                                  |
  +------------------------+-------+-------------+-----------------------------------------------------------------------------------------+
  | app:framePollInterval  |       | 1000        | How often to check for a frame update in microseconds                                   |
  +------------------------+-------+-------------+-----------------------------------------------------------------------------------------+
  | app:allowDMA           |       | yes         | Allow direct DMA transfers if supported (see `README.md` in the `module` dir)           |
  +------------------------+-------+-------------+-----------------------------------------------------------------------------------------+
  | app:shmFile            | -f    | /dev/kvmfr0 | The path to the shared memory file, or the name of the kvmfr device to use, e.g. kvmfr0 |
  +------------------------+-------+-------------+-----------------------------------------------------------------------------------------+

  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | Long                    | Short | Value                  | Description                                                          |
  +=========================+=======+========================+======================================================================+
  | win:title               |       | Looking Glass (client) | The window title                                                     |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:position            |       | center                 | Initial window position at startup                                   |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:size                |       | 1024x768               | Initial window size at startup                                       |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:autoResize          | -a    | no                     | Auto resize the window to the guest                                  |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:allowResize         | -n    | yes                    | Allow the window to be manually resized                              |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:keepAspect          | -r    | yes                    | Maintain the correct aspect ratio                                    |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:forceAspect         |       | yes                    | Force the window to maintain the aspect ratio                        |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:dontUpscale         |       | no                     | Never try to upscale the window                                      |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:intUpscale          |       | no                     | Allow only integer upscaling                                         |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:shrinkOnUpscale     |       | no                     | Limit the window dimensions when dontUpscale is enabled              |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:borderless          | -d    | no                     | Borderless mode                                                      |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:fullScreen          | -F    | no                     | Launch in fullscreen borderless mode                                 |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:maximize            | -T    | no                     | Launch window maximized                                              |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:minimizeOnFocusLoss |       | no                     | Minimize window on focus loss                                        |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:fpsMin              | -K    | -1                     | Frame rate minimum (0 = disable - not recommended, -1 = auto detect) |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:ignoreQuit          | -Q    | no                     | Ignore requests to quit (i.e. Alt+F4)                                |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:noScreensaver       | -S    | yes                    | Prevent the screensaver from starting                                |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:autoScreensaver     |       | no                     | Prevent the screensaver from starting when guest requests it         |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:alerts              | -q    | yes                    | Show on screen alert messages                                        |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:quickSplash         |       | no                     | Skip fading out the splash screen when a connection is established   |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:overlayDimsDesktop  |       | no                     | Dim the desktop when in interactive overlay mode                     |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:rotate              |       | 0                      | Rotate the displayed image (0, 90, 180, 270)                         |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:uiFont              |       | DejaVu Sans Mono       | The font to use when rendering on-screen UI                          |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:uiSize              |       | 14                     | The font size to use when rendering on-screen UI                     |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:jitRender           |       | no                     | Enable just-in-time rendering                                        |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:requestActivation   |       | yes                    | Request activation when attention is needed                          |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:showFPS             | -k    | no                     | Enable the FPS & UPS display                                         |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+

  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | Long                         | Short | Value               | Description                                                                      |
  +==============================+=======+=====================+==================================================================================+
  | input:captureOnFocus         |       | no                  | Enable capture mode when the window becomes focused                              |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:grabKeyboard           | -G    | yes                 | Grab the keyboard in capture mode                                                |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:grabKeyboardOnFocus    |       | no                  | Grab the keyboard when focused                                                   |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:releaseKeysOnFocusLoss |       | yes                 | On focus loss, send key up events to guest for all held keys                     |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:escapeKey              | -m    | 70 = KEY_SCROLLLOCK | Specify the escape/menu key to use (use "help" to see valid values)              |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:ignoreWindowsKeys      |       | no                  | Do not pass events for the windows keys to the guest                             |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:hideCursor             | -M    | yes                 | Hide the local mouse cursor                                                      |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:mouseSens              |       | 0                   | Initial mouse sensitivity when in capture mode (-9 to 9)                         |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:mouseSmoothing         |       | yes                 | Apply simple mouse smoothing when rawMouse is not in use (helps reduce aliasing) |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:rawMouse               |       | yes                 | Use RAW mouse input when in capture mode (good for gaming)                       |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:mouseRedraw            |       | yes                 | Mouse movements trigger redraws (ignores FPS minimum)                            |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:autoCapture            |       | no                  | Try to keep the mouse captured when needed                                       |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:captureOnly            |       | no                  | Only enable input via SPICE if in capture mode                                   |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:helpMenuDelay          |       | 200                 | Show help menu after holding down the escape key for this many milliseconds      |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+

  +------------------------+-------+-----------------------------------+---------------------------------------------------------------------+
  | Long                   | Short | Value                             | Description                                                         |
  +========================+=======+===================================+=====================================================================+
  | spice:enable           | -s    | yes                               | Enable the built in SPICE client for input and/or clipboard support |
  +------------------------+-------+-----------------------------------+---------------------------------------------------------------------+
  | spice:host             | -c    | /opt/PVM/vms/Windows/windows.sock | The SPICE server host or UNIX socket                                |
  +------------------------+-------+-----------------------------------+---------------------------------------------------------------------+
  | spice:port             | -p    | 0                                 | The SPICE server port (0 = unix socket)                             |
  +------------------------+-------+-----------------------------------+---------------------------------------------------------------------+
  | spice:input            |       | yes                               | Use SPICE to send keyboard and mouse input events to the guest      |
  +------------------------+-------+-----------------------------------+---------------------------------------------------------------------+
  | spice:clipboard        |       | yes                               | Use SPICE to synchronize the clipboard contents with the guest      |
  +------------------------+-------+-----------------------------------+---------------------------------------------------------------------+
  | spice:clipboardToVM    |       | yes                               | Allow the clipboard to be synchronized TO the VM                    |
  +------------------------+-------+-----------------------------------+---------------------------------------------------------------------+
  | spice:clipboardToLocal |       | yes                               | Allow the clipboard to be synchronized FROM the VM                  |
  +------------------------+-------+-----------------------------------+---------------------------------------------------------------------+
  | spice:audio            |       | yes                               | Enable SPICE audio support                                          |
  +------------------------+-------+-----------------------------------+---------------------------------------------------------------------+
  | spice:scaleCursor      | -j    | yes                               | Scale cursor input position to screen size when up/down scaled      |
  +------------------------+-------+-----------------------------------+---------------------------------------------------------------------+
  | spice:captureOnStart   |       | no                                | Capture mouse and keyboard on start                                 |
  +------------------------+-------+-----------------------------------+---------------------------------------------------------------------+
  | spice:alwaysShowCursor |       | no                                | Always show host cursor                                             |
  +------------------------+-------+-----------------------------------+---------------------------------------------------------------------+
  | spice:showCursorDot    |       | yes                               | Use a "dot" cursor when the window does not have focus              |
  +------------------------+-------+-----------------------------------+---------------------------------------------------------------------+
  | spice:largeCursorDot   |       | yes                               | Use a larger version of the "dot" cursor                            |
  +------------------------+-------+-----------------------------------+---------------------------------------------------------------------+

  +------------------------+-------+-------+-------------------------------------------------------------------------------+
  | Long                   | Short | Value | Description                                                                   |
  +========================+=======+=======+===============================================================================+
  | audio:periodSize       |       | 256   | Requested audio device period size in samples                                 |
  +------------------------+-------+-------+-------------------------------------------------------------------------------+
  | audio:bufferLatency    |       | 12    | Additional buffer latency in milliseconds                                     |
  +------------------------+-------+-------+-------------------------------------------------------------------------------+
  | audio:micDefault       |       | allow | Default action when an application opens the microphone (prompt, allow, deny) |
  +------------------------+-------+-------+-------------------------------------------------------------------------------+
  | audio:micShowIndicator |       | yes   | Display microphone usage indicator                                            |
  +------------------------+-------+-------+-------------------------------------------------------------------------------+
  | audio:syncVolume       |       | yes   | Synchronize the volume level with the guest                                   |
  +------------------------+-------+-------+-------------------------------------------------------------------------------+

  +-------------------+-------+-------+---------------------------------------------------------------------------+
  | Long              | Short | Value | Description                                                               |
  +===================+=======+=======+===========================================================================+
  | egl:vsync         |       | no    | Enable vsync                                                              |
  +-------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:doubleBuffer  |       | no    | Enable double buffering                                                   |
  +-------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:multisample   |       | yes   | Enable Multisampling                                                      |
  +-------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:nvGainMax     |       | 1     | The maximum night vision gain                                             |
  +-------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:nvGain        |       | 0     | The initial night vision gain at startup                                  |
  +-------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:cbMode        |       | 0     | Color Blind Mode (0 = Off, 1 = Protanope, 2 = Deuteranope, 3 = Tritanope) |
  +-------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:scale         |       | 0     | Set the scale algorithm (0 = auto, 1 = nearest, 2 = linear)               |
  +-------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:debug         |       | no    | Enable debug output                                                       |
  +-------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:noBufferAge   |       | no    | Disable partial rendering based on buffer age                             |
  +-------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:noSwapDamage  |       | no    | Disable swapping with damage                                              |
  +-------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:scalePointer  |       | yes   | Keep the pointer size 1:1 when downscaling                                |
  +-------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:mapHDRtoSDR   |       | yes   | Map HDR content to the SDR color space                                    |
  +-------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:peakLuminance |       | 250   | The peak luminance level in nits for HDR to SDR mapping                   |
  +-------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:maxCLL        |       | 10000 | Maximum content light level in nits for HDR to SDR mapping                |
  +-------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:preset        |       | NULL  | The initial filter preset to load                                         |
  +-------------------+-------+-------+---------------------------------------------------------------------------+

  +----------------------+-------+-------+---------------------------------------------+
  | Long                 | Short | Value | Description                                 |
  +======================+=======+=======+=============================================+
  | opengl:mipmap        |       | yes   | Enable mipmapping                           |
  +----------------------+-------+-------+---------------------------------------------+
  | opengl:vsync         |       | no    | Enable vsync                                |
  +----------------------+-------+-------+---------------------------------------------+
  | opengl:preventBuffer |       | yes   | Prevent the driver from buffering frames    |
  +----------------------+-------+-------+---------------------------------------------+
  | opengl:amdPinnedMem  |       | yes   | Use GL_AMD_pinned_memory if it is available |
  +----------------------+-------+-------+---------------------------------------------+

  +-----------------------+-------+-------+-------------------------+
  | Long                  | Short | Value | Description             |
  +=======================+=======+=======+=========================+
  | wayland:warpSupport   |       | yes   | Enable cursor warping   |
  +-----------------------+-------+-------+-------------------------+
  | wayland:fractionScale |       | yes   | Enable fractional scale |
  +-----------------------+-------+-------+-------------------------+

  +---------------------+-------+-------+----------------------------------------------------------+
  | Long                | Short | Value | Description                                              |
  +=====================+=======+=======+==========================================================+
  | i3:globalFullScreen |       | yes   | Use i3's global full screen feature (spans all monitors) |
  +---------------------+-------+-------+----------------------------------------------------------+

  +--------------------+-------+---------------+------------------------------------+
  | Long               | Short | Value         | Description                        |
  +====================+=======+===============+====================================+
  | pipewire:outDevice |       | Looking Glass | The default playback device to use |
  +--------------------+-------+---------------+------------------------------------+
  | pipewire:recDevice |       | PureNoise Mic | The default record device to use   |
  +--------------------+-------+---------------+------------------------------------+

.. _host_usage:

Host usage
----------

By default the host application will simply work however there are some
configurable options available. While the host application will accept command
line arguments, it is more convenient to create a ``looking-glass-host.ini``
config file for persistent configuration changes.

This file must be placed in the same directory as the Looking Glass host, by
default ``C:\Program&nbsp;Files\Looking&nbsp;Glass&nbsp;(host)\``.

.. _host_capture:

Capture interface
~~~~~~~~~~~~~~~~~

.. note::
  Currently we only provide support for the Windows host application, Linux
  options are not currently documented.

Currently under windows there are three capture interfaces available for use,
by default the most compatible and commonly supported interface is selected
however this can be changed via the ini file with the following configuration:

.. code:: ini

 [app]
 capture=<INTERFACE>

Where ``<INTERFACE>`` is one of ``d12``, ``dxgi`` or ``nvfbc``

DXGI Desktop Duplication Caveat
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Due to the design of Microsoft's DXGI API and the decision made to roll
hardware cursor updates into the capture stream this interface can suffer from
microstutters when the mouse is being moved/updated. This issue only affects
guest applications that make use of the hardware cursor instead of compositing
the cursor directly, as such titles that do not use a mouse (most FPV games)
are not affected.

Most people will not even notice this, but it needs to be said for those that
do so that we do not get flooded with support requests for something we can not
fix.

.. _host_capture_d12:

DirectX 12 DXGI Desktop Duplication
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

This interface (``D12``) is the default and most performant capture interface
for Windows 10 and later having been introduced with the Beta 7 release of
Looking Glass. Impressively this new capture engine is faster then NvFBC, and
has fewer overheads. This is because this interface can capture and download
the captured frames directly from the GPU into the shared memory interface.

D12 Configuration Options
"""""""""""""""""""""""""

* ``adapter`` - The name of the specific adapter you wish to capture

* ``output`` - The name of the specific output you wish to capture

* ``trackDamage`` - Default enabled, this saves bandwidth by only updating and
  transferring the regions of the capture that have changed since the last
  frame.

* ``debug`` - Enables DirectX 12 debugging and validation, only enable this if
  you're having problems and have been told to do so by our support team. Note
  that you must have the DirectX SDK installed for this to work.

* ``downsample`` - See :ref:`host_downsampling`

* ``HDR16to10`` - Converts HDR16/8bpp content to HDR10/4bpp to save bandwidth.
  Note that this incurs additional overheads in the guest and may decrease
  performance. Default enabled, but only active if HDR is enabled in Windows.

* ``allowRGB24`` - Losslessly packs 32-bit RGBA8 content into 24-bit RGB by
  omitting the unused alpha channel. This saves bandwidth but requires
  additional processing so may not yield a performance increase. Might be
  helpful if you're already bandwidth constrained. Default disabled.

.. _host_capture_dxgi:

DirectX 11 DXGI Desktop Duplication
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

This interface (``DXGI``) is the most compatible capture interface for Windows,
unfortunately though it does suffer from several drawbacks over other options.
If the DirectX 12 (``D12``) capture interface fails to initialize Looking Glass
will automatically fall back to using this capture interface instead.

Due to the design of Microsoft's DXGI API and the decision made to roll
hardware cursor updates into the capture stream this interface can suffer from
microstutters when the mouse is being moved/updated. This issue only affects
guest applications that make use of the hardware cursor instead of compositing
the cursor directly, as such titles that do not use a mouse (most FPV games)
are not affected.

The other drawback of this API is the overall system overhead as it requires
copying the captured frames into a staging buffer before it is copied into the
shared memory area.

DXGI Configuration Options
""""""""""""""""""""""""""

* ``adapter`` - The name of the specific adapter you wish to capture

* ``output`` - The name of the specific output you wish to capture

* ``maxTextures`` - The maximum number of frames to buffer before skipping
  frames. Default is 4 however realistically the this limit should never be
  reached unless the Looking Glass client application is not keeping up.

* ``useAcquireLock`` - Enable locking around ``AcquireNextFrame``. This is an
  experimental feature and should be left enabled if you're not sure. Default
  is enabled.

* ``dwmFlush`` - Use ``DwmFlush`` to sync the capture to the windows
  presentation interval. This is experimental and may degrade performance.
  Default is disabled.

* ``disableDamage`` - Default is false. This disables damage tracking which
  normally would save bandwidth by only updating and transferring the regions
  of the capture that have changed since the last frame.

* ``debug`` - Enables DirectX 11 debugging, only enable this if you're having
  problems and have been told to do so by our support team. Note that you must
  have the DirectX SDK installed for this to work.

* ``allowRGB24`` - Losslessly packs 32-bit RGBA8 content into 24-bit RGB by
  omitting the unused alpha channel. This saves bandwidth but requires
  additional processing so may not yield a performance increase. Might be
  helpful if you're already bandwidth constrained. Default enabled.

* ``downsample`` - See :ref:`host_downsampling`

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

.. note::
   As of Looking Glass Beta 7, the D12 capture interface is faster then NvFBC
   while also reducing the memory bandwidth requirements. We recommend
   migrating to D12 if at all possible.

To enable its usage use the following configuration in the
``looking-glass-host.ini`` file:

.. code:: ini

  [app]
  capture=nvfbc

If this feature is unavailable to you the host application will fail to start
and the host log will contain an error stating that the feature is not
available.

NvFBC Configuration Options
"""""""""""""""""""""""""""

* ``decoupleCursor`` - This option prevents the cursor from being composited
  before capture onto the captured frame, and instead sends cursor updates to
  the client independent of frame updates. Default is true.

* ``diffRes`` - The resolution of the differential map, see the NvFBC capture
  SDK documentation for more information. Default is 128.

* ``adapterIndex`` - If you have multiple adapters, you can select which to use
  with this option. Default is to select the first valid device.

* ``dwmFlush`` - Use ``DwmFlush`` to sync the capture to the windows
  presentation interval. This is experimental and may degrade performance.
  Default is disabled.

* ``noHDR`` - Force NvFBC to capture HDR content as SDR. Default is enabled.

* ``downsample`` - See :ref:`host_downsampling`


This capture interface also looks for and reads the value of the system
environment variable ``NVFBC_PRIV_DATA`` if it has been set, documentation on
its usage however is unavailable (Google is your friend).

.. _host_select_ivshmem:

Selecting an IVSHMEM device
~~~~~~~~~~~~~~~~~~~~~~~~~~~
For those attaching multiple IVSHMEM devices to their Virtual Machines, you must
configure the Looking Glass host to use the correct device. By default the first
device is selected.

The ``os:shmDevice`` option configures which device is used. These are ordered
by PCI slot and count up from 0 (default), with 0 being the first IVSHMEM device
in the lowest slot.

.. code:: ini

  [os]
  ; Select the second IVSHMEM device
  shmDevice=1

.. note::
   ``os:shmDevice`` ignores the actual PCI slot number, instead selecting the
   *N*\th slot occupied by an IVSHMEM device. For example: with only two IVSHMEM
   devices in slots 0x03 and 0x05, the device in slot 0x03 will be referred to
   by *0* (first shm device), and the device in 0x05 by *1* (second shm device).

PCI slot numbers are visible in Device Manager:

1. Double-click any "IVSHMEM device" in Device Manager (``devmgmt.msc``)
2. Find the slot number in the "Location:" field. (e.g. PCI slot 5)

You can also find a listing of IVSHMEM devices in the ``looking-glass-host.txt``
log file, with slot numbers shown next to "device" (asterisk indicates currently
selected device)::

   [I]     19989544      …      IVSHMEM 0  on bus 0x6, device 0x3, function 0x0
   [I]     19990438      …      IVSHMEM 1* on bus 0x6, device 0x5, function 0x0

.. _host_downsampling:

Downsampling
~~~~~~~~~~~~

The host application is able to downsample the captured frame before transfer
to the client application, this provides an opportunity to save some bandwidth
on memory constrained systems. It also makes it possible to run the guest at a
substantially higher resolution then your actual monitor for a super scaling
type effect, without having to incur the bandwidth penalty that would normally
occur when doing this.

The configuration for this is fairly straightforward and is defined as set of
rules to determine when to perform this downsampling. The format is as follows:

.. code::

   (>|>=)(WIDTH)x(HEIGHT):(TARGET WIDTH)x(TARGET HEIGHT)

**Examples:**

.. code:: ini

  ; Downsample exactly 3840x2160 to 1920x1080
  downsample=3840x2160:1920x1080

  ; Downsample anything greater then 1920x1080 to 1920x1080
  downsample=>1920x1080:1920x1080

  ; Downsample 3840x2160 to 1920x1080, or 3840x2400 to 1920x1200
  downsample=3840x2160:1920x1080,3840x2400:1920x1200
