.. _installing:

Installation
############

.. _client_install:

Client
------

The Looking Glass Client receives frames from the :ref:`Host <host_install>` to
display on your screen. It also handles input, and can optionally share the
system clipboard with your guest OS through Spice.

First you must build the client from source code, see :ref:`building`.

.. _client_libvirt_configuration:

libvirt Configuration
~~~~~~~~~~~~~~~~~~~~~

This article assumes you already have a fully functional libvirt domain with
PCI passthrough working on a dedicated monitor.

If you use virt-manager, this guide also applies to you, since virt-manager uses
libvirt as its back-end.

**If you are using QEMU directly, this does not apply to you.**

Add the following to your libvirt machine configuration inside the
'devices' section by running ``virsh edit <VM>`` where ``<VM>`` is the name of
your virtual machine.

.. code:: xml

   <shmem name='looking-glass'>
     <model type='ivshmem-plain'/>
     <size unit='M'>32</size>
   </shmem>

The memory size (show as 32 in the example above) may need to be
adjusted as per the :ref:`Determining Memory <client_determining_memory>` section.

.. _client_spice_server:

Spice Server
^^^^^^^^^^^^

If you would like to use Spice to give you keyboard and mouse input
along with clipboard sync support, make sure you have a
``<graphics type='spice'>`` device, then:

-  Find your ``<video>`` device, and set ``<model type='none'/>``

   -  If you can't find it, make sure you have a ``<graphics>``
      device, save and edit again
   -  On older libvirt versions, just disable the device in Windows
      Device Manager

-  Remove the ``<input type='tablet'/>`` device, if you have one
-  Create an ``<input type='mouse'/>`` device, if you don't already have one
-  Create an ``<input type='keyboard' bus='virtio'/>`` device to improve
   keyboard usage

   -  This requires the *vioinput* driver from
      `virtio-win <https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/stable-virtio/>`_
      to be installed in the guest

If you want clipboard synchronization please see
:ref:`client_clipboard_synchronization`

.. _client_apparmor:

AppArmor
^^^^^^^^

For libvirt versions before **5.10.0**, if you are using AppArmor, you
need to add permissions for QEMU to access the shared memory file. This
can be done by adding the following to
``/etc/apparmor.d/local/abstractions/libvirt-qemu``::

   /dev/shm/looking-glass rw,

then, restart AppArmor.

.. code:: bash

   sudo systemctl restart apparmor

.. _client_memballoon_tweak:

Memballoon
^^^^^^^^^^

The VirtIO memballoon device enables the host to dynamically reclaim memory
from your VM by growing the balloon inside the guest, reserving reclaimed
memory. Libvirt adds this device to guests by default.

However, this device causes major performance issues with VFIO passthrough
setups, and should be disabled.

Find the ``<memballoon>`` tag and set its type to ``none``:

.. code:: xml

   <memballoon model="none"/>

.. _client_qemu_commands:

QEMU Commands
~~~~~~~~~~~~~

**If you are using libvirt/virt-manager, then this does not apply to you.**

Add the following to the commands to your QEMU command line, adjusting
the ``bus`` parameter to suit your particular configuration:

.. code:: bash

   -device ivshmem-plain,memdev=ivshmem,bus=pcie.0 \
   -object memory-backend-file,id=ivshmem,share=on,mem-path=/dev/shm/looking-glass,size=32M

The memory size (shown as 32M in the example above) may need to be
adjusted as per :ref:`Determining Memory <client_determining_memory>` section.

.. _client_determining_memory:

Determining Memory
~~~~~~~~~~~~~~~~~~

You will need to adjust the memory size to be suitable for
your desired maximum resolution, with the following formula:

``width x height x 4 x 2 = total bytes``

``total bytes / 1024 / 1024 = total megabytes + 10``

For example, for a resolution of 1920x1080 (1080p):

``1920 x 1080 x 4 x 2 = 16,588,800 bytes``

``16,588,800 / 1024 / 1024 = 15.82 MB + 10 = 25.82 MB``

You must round this value up to the nearest power of two, which for the
provided example is 32MB.

.. _client_shmfile_permissions:

Shared Memory File Permissions
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The shared memory file used by IVSHMEM is found in ``/dev/shm/looking-glass``.
By default, it is owned by QEMU, and does not give read/write permissions to
your user, which are required for Looking Glass to run properly.

You can use `systemd-tmpfiles` to create the file before running your VM,
granting the necessary permissions which allow Looking Glass to use the file
properly.

Create a new file ``/etc/tmpfiles.d/10-looking-glass.conf``, and populate it
with the following::

   #Type Path               Mode UID  GID Age Argument

   f /dev/shm/looking-glass 0660 user kvm -

Change ``UID`` to the user name you will run Looking Glass with, usually your
own.

.. _client_clipboard_synchronization:

Clipboard Synchronization
~~~~~~~~~~~~~~~~~~~~~~~~~

Looking Glass can synchronize the clipboard between the host and guest using
the SPICE guest agent.

1. Install the SPICE guest tools from
https://www.spice-space.org/download.html#windows-binaries.

2. Configure your VM to enable the SPICE guest agent:

-  QEMU

.. code:: bash

   -device virtio-serial-pci \
   -chardev spicevmc,id=vdagent,name=vdagent \
   -device virtserialport,chardev=vdagent,name=com.redhat.spice.0

-  libvirt

.. code:: xml

     <channel type="spicevmc">
       <target type="virtio" name="com.redhat.spice.0"/>
       <address type="virtio-serial" controller="0" bus="0" port="1"/>
     </channel>
     <!-- No need to add a VirtIO Serial device, it will be added automatically -->

.. _client_usage:

Usage
-----

The client command is the binary file: **looking-glass-client**. This
command should run after the Windows Host Application has started.

You may run the client directly from the build directory. Alternatively, to
install the client for all users, you can run
::

   make install

To install for the local user only, run
::

   cmake -DCMAKE_INSTALL_PREFIX=~/.local .. && make install

.. _client_key_bindings:

Default Key Bindings
~~~~~~~~~~~~~~~~~~~~

By default, Looking Glass uses the :kbd:`Scroll Lock` key as the escape key
for commands, as well as the input :kbd:`capture` mode toggle; this can be
changed using the ``-m`` switch if you desire a different key. Below are
a list of current key bindings:

============================ =======================================================
Command                      Description
============================ =======================================================
:kbd:`ScrLk`                 Toggle capture mode
:kbd:`ScrLk` + :kbd:`Q`      Quit
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
:kbd:`ScrLk` + :kbd:`Insert` Increase mouse sensitivity in capture mode
:kbd:`ScrLk` + :kbd:`Del`    Decrease mouse sensitivity in capture mode
:kbd:`ScrLk` + :kbd:`LWin`   Send :kbd:`LWin` to the guest
:kbd:`ScrLk` + :kbd:`RWin`   Send :kbd:`RWin` to the guest
============================ =======================================================

You can also find this list at any time by holding down :kbd:`Scroll Lock`.

.. _client_cli_options:

Command Line Options
~~~~~~~~~~~~~~~~~~~~

A full list of command line options is available with the ``--help`` or ``-h``
options.

Example: ``looking-glass-client --help``

Common options include ``-s`` for disabling spice, ``-S`` for disabling the
screen saver, and ``-F`` to automatically enter full screen.

Options may be provided with a short form, if available, or long form.
Boolean options may be specified without a parameter to toggle their
state.

Examples:

- ``looking-glass-client -F`` (short)
- ``looking-glass-client win:fullScreen`` (long)
- ``looking-glass-client -f /dev/shm/my-lg-shmem`` (short with parameter)
- ``looking-glass-client app:shmFile=/dev/shm/my-lg-shmem`` (long with parameter)

.. _client_config_options_file:

Configuration Files
~~~~~~~~~~~~~~~~~~~

By default, Looking Glass will load config files from
the following locations:

-  /etc/looking-glass-client.ini
-  ~/.looking-glass-client.ini
-  $XDG_CONFIG_HOME/looking-glass/client.ini (usually ~/.config/looking-glass/client.ini)

All config files are loaded in order. Duplicate entries override earlier ones.
This means you can set a system-wide configuration in
``/etc/looking-glass-client.ini``, and override specific options for just
your user in ``~/.looking-glass-client.ini``, which is overlayed on top of
the system-wide configuration.

When first launched, the Looking-Glass client will create the folder
$XDG_CONFIG_HOME/looking-glass/ if it does not yet exist.

The format of config files is the commonly known INI format, for example::

   [win]
   fullScreen=yes

   [egl]
   nvGain=1

Command line arguments will override any options loaded from config
files.

.. _client_overlay_mode:

Overlay Mode
~~~~~~~~~~~~

The Overlay Mode lets you configure various runtime options for Looking Glass.
These include:

- EGL filters
- Performance metrics options
- Debug frame damage display

(see :ref:`client_config_widget`)

You can also reposition and resize enabled widgets, like the FPS/UPS Display,
and Performance Metrics.

Enter and exit Overlay Mode with :kbd:`ScrLk` + :kbd:`O`.
:kbd:`ESC` can also be used to exit. (see :ref:`client_key_bindings`)

Modifications done to widgets in Overlay Mode are stored in
``$XDG_CONFIG_HOME/looking-glass/imgui.ini``.
Please do not manually edit this file while Looking Glass is running,
as your changes may be discarded.

.. _client_config_widget:

Configuration Widget
~~~~~~~~~~~~~~~~~~~~

The Configuration Widget is accessible through the Overlay Mode. The
widget has multiple tabs that allow setting a variety of modes and
parameters for Looking Glass at runtime.

Settings tab
^^^^^^^^^^^^

- Performance Metrics: A toggle for the Performance Metrics Widget.
  Multiple graphs are available, and they will stack vertically.
- EGL: Modify EGL features, such as the algorithm used for scaling, and
  night vision mode.

Changes in the Settings tab are not persistent, and will change back to
their default values when the client is restarted.

EGL Filters tab
^^^^^^^^^^^^^^^

The EGL Filters tab contains options for toggling, configuring, and ordering 
post-processing filters. Each filter can be expanded to open its settings. 
Filters can also be re-ordered by dragging them up or down. Filters are applied 
from top to bottom, keep this in mind when ordering them, e.g applying CAS
before FSR might have different results than the reverse. Users are encouraged
to experiment with the order and parameters to achieve optimal results. The 
currently available filters include:

-  Downscaler: Filter for downscaling the host resolution. Can be used to undo 
   poor upscaling on the VM to better utilize AMD FSR (see below). The filter 
   has a pixel-size setting that is used to set the effective downscaling ratio,
   and a configurable interpolation algorithm.

-  AMD FidelityFX Super Resolution (FSR): Spatial upscaling filter that works
   on low resolution frames from the guest VM and intelligently upscales to a
   higher resolution. The filter sharpness is tunable, and displays the
   equivalent AMD quality mode based on the resolution difference.

-  AMD FidelityFX Contrast Adaptive Sharpening (CAS): Filter that
   increases visual quality by applying a sharpening algorithm to the
   video. CAS can sometimes restore detail lost in a typical upscaling
   application. Has adjustable sharpness setting.

The filter settings and order can be saved to presets so that it can be restored
at a later time. As filter settings are usually application specific, multiple 
presets can be defined for each case scenario. To save a preset, click on "Save 
preset as..." and enter a preset name. Presets are loaded by selecting them in 
the "Preset name" pull down. Presets are persistent and are stored on disk at
``$XDG_CONFIG_HOME/looking-glass/presets``.

.. warning::
   Please refrain from modifying any files under the ``presets`` folder.
   Those files are meant to be modified only by the Looking-Glass client.

.. note::
   Although presets are persistent, the client will not remember which
   preset was used last session, so a preset needs to be recalled once
   the client starts.

.. _client_full_command_options:

Full Command Line Options
~~~~~~~~~~~~~~~~~~~~~~~~~

The following is a complete list of options accepted by this application

  +------------------------+-------+------------------------+-----------------------------------------------------------------------------------------+
  | Long                   | Short | Value                  | Description                                                                             |
  +========================+=======+========================+=========================================================================================+
  | app:configFile         | -C    | NULL                   | A file to read additional configuration from                                            |
  +------------------------+-------+------------------------+-----------------------------------------------------------------------------------------+
  | app:renderer           | -g    | auto                   | Specify the renderer to use                                                             |
  +------------------------+-------+------------------------+-----------------------------------------------------------------------------------------+
  | app:license            | -l    | no                     | Show the license for this application and then terminate                                |
  +------------------------+-------+------------------------+-----------------------------------------------------------------------------------------+
  | app:cursorPollInterval |       | 1000                   | How often to check for a cursor update in microseconds                                  |
  +------------------------+-------+------------------------+-----------------------------------------------------------------------------------------+
  | app:framePollInterval  |       | 1000                   | How often to check for a frame update in microseconds                                   |
  +------------------------+-------+------------------------+-----------------------------------------------------------------------------------------+
  | app:allowDMA           |       | yes                    | Allow direct DMA transfers if supported (see `README.md` in the `module` dir)           |
  +------------------------+-------+------------------------+-----------------------------------------------------------------------------------------+
  | app:shmFile            | -f    | /dev/shm/looking-glass | The path to the shared memory file, or the name of the kvmfr device to use, e.g. kvmfr0 |
  +------------------------+-------+------------------------+-----------------------------------------------------------------------------------------+

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
  | win:noScreensaver       | -S    | no                     | Prevent the screensaver from starting                                |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:autoScreensaver     |       | no                     | Prevent the screensaver from starting when guest requests it         |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:alerts              | -q    | yes                    | Show on screen alert messages                                        |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:quickSplash         |       | no                     | Skip fading out the splash screen when a connection is established   |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:rotate              |       | 0                      | Rotate the displayed image (0, 90, 180, 270)                         |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:uiFont              |       | DejaVu Sans Mono       | The font to use when rendering on-screen UI                          |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:uiSize              |       | 14                     | The font size to use when rendering on-screen UI                     |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:jitRender           |       | no                     | Enable just-in-time rendering                                        |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:showFPS             | -k    | no                     | Enable the FPS & UPS display                                         |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+

  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | Long                         | Short | Value               | Description                                                                      |
  +==============================+=======+=====================+==================================================================================+
  | input:grabKeyboard           | -G    | yes                 | Grab the keyboard in capture mode                                                |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:grabKeyboardOnFocus    |       | no                  | Grab the keyboard when focused                                                   |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:releaseKeysOnFocusLoss |       | yes                 | On focus loss, send key up events to guest for all held keys                     |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:escapeKey              | -m    | 70 = KEY_SCROLLLOCK | Specify the escape key, see <linux/input-event-codes.h> for valid values         |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:ignoreWindowsKeys      |       | no                  | Do not pass events for the windows keys to the guest                             |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:hideCursor             | -M    | yes                 | Hide the local mouse cursor                                                      |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:mouseSens              |       | 0                   | Initial mouse sensitivity when in capture mode (-9 to 9)                         |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:mouseSmoothing         |       | yes                 | Apply simple mouse smoothing when rawMouse is not in use (helps reduce aliasing) |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:rawMouse               |       | no                  | Use RAW mouse input when in capture mode (good for gaming)                       |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:mouseRedraw            |       | yes                 | Mouse movements trigger redraws (ignores FPS minimum)                            |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:autoCapture            |       | no                  | Try to keep the mouse captured when needed                                       |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:captureOnly            |       | no                  | Only enable input via SPICE if in capture mode                                   |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:helpMenuDelay          |       | 200                 | Show help menu after holding down the escape key for this many milliseconds      |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+

  +------------------------+-------+-----------+---------------------------------------------------------------------+
  | Long                   | Short | Value     | Description                                                         |
  +========================+=======+===========+=====================================================================+
  | spice:enable           | -s    | yes       | Enable the built in SPICE client for input and/or clipboard support |
  +------------------------+-------+-----------+---------------------------------------------------------------------+
  | spice:host             | -c    | 127.0.0.1 | The SPICE server host or UNIX socket                                |
  +------------------------+-------+-----------+---------------------------------------------------------------------+
  | spice:port             | -p    | 5900      | The SPICE server port (0 = unix socket)                             |
  +------------------------+-------+-----------+---------------------------------------------------------------------+
  | spice:input            |       | yes       | Use SPICE to send keyboard and mouse input events to the guest      |
  +------------------------+-------+-----------+---------------------------------------------------------------------+
  | spice:clipboard        |       | yes       | Use SPICE to synchronize the clipboard contents with the guest      |
  +------------------------+-------+-----------+---------------------------------------------------------------------+
  | spice:clipboardToVM    |       | yes       | Allow the clipboard to be synchronized TO the VM                    |
  +------------------------+-------+-----------+---------------------------------------------------------------------+
  | spice:clipboardToLocal |       | yes       | Allow the clipboard to be synchronized FROM the VM                  |
  +------------------------+-------+-----------+---------------------------------------------------------------------+
  | spice:scaleCursor      | -j    | yes       | Scale cursor input position to screen size when up/down scaled      |
  +------------------------+-------+-----------+---------------------------------------------------------------------+
  | spice:captureOnStart   |       | no        | Capture mouse and keyboard on start                                 |
  +------------------------+-------+-----------+---------------------------------------------------------------------+
  | spice:alwaysShowCursor |       | no        | Always show host cursor                                             |
  +------------------------+-------+-----------+---------------------------------------------------------------------+
  | spice:showCursorDot    |       | yes       | Use a "dot" cursor when the window does not have focus              |
  +------------------------+-------+-----------+---------------------------------------------------------------------+

  +------------------+-------+-------+---------------------------------------------------------------------------+
  | Long             | Short | Value | Description                                                               |
  +==================+=======+=======+===========================================================================+
  | egl:vsync        |       | no    | Enable vsync                                                              |
  +------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:doubleBuffer |       | no    | Enable double buffering                                                   |
  +------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:multisample  |       | yes   | Enable Multisampling                                                      |
  +------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:nvGainMax    |       | 1     | The maximum night vision gain                                             |
  +------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:nvGain       |       | 0     | The initial night vision gain at startup                                  |
  +------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:cbMode       |       | 0     | Color Blind Mode (0 = Off, 1 = Protanope, 2 = Deuteranope, 3 = Tritanope) |
  +------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:scale        |       | 0     | Set the scale algorithm (0 = auto, 1 = nearest, 2 = linear)               |
  +------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:debug        |       | no    | Enable debug output                                                       |
  +------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:noBufferAge  |       | no    | Disable partial rendering based on buffer age                             |
  +------------------+-------+-------+---------------------------------------------------------------------------+
  | egl:noSwapDamage |       | no    | Disable swapping with damage                                              |
  +------------------+-------+-------+---------------------------------------------------------------------------+

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

.. _host_install:

Host
----

The Looking Glass Host captures frames from the guest OS using a capture API,
and sends them to the :ref:`Client <client_install>`—be it on the host OS (hypervisor)
or another Virtual Machine—through a low-latency transfer protocol over shared
memory.

You can get the host program in two ways:

-  Download a pre-built binary from https://looking-glass.io/downloads
   (**recommended**)

-  Download the source code as described in :ref:`building`, then
   :ref:`build the host <host_building>`.

.. _host_install_windows:

Windows
~~~~~~~

To begin, you must first run the Windows VM with the changes noted above in
either the :ref:`client_libvirt_configuration` or :ref:`client_qemu_commands`
sections.

.. _installing_the_ivshmem_driver:

Installing the IVSHMEM Driver
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Windows will not prompt for a driver for the IVSHMEM device, instead, it
will use a default null (do nothing) driver for the device. To install
the IVSHMEM driver you will need to go into the device manager and
update the driver for the device "PCI standard RAM Controller" under the
"System Devices" node.

A signed Windows 10 driver can be obtained from Red Hat for this device
from the below address:

https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/upstream-virtio/

Please note that you must obtain version 0.1.161 or later.

If you encounter warnings or errors about driver signatures, ensure secure boot
is turned off in the bios/UEFI settings of your virtual machine.

.. _host_install_service:

Installing the Looking Glass Service
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

After installing your IVSHMEM driver, we can now install the Looking Glass Host
onto our Windows Virtual Machine.

1. First, run ``looking-glass-host-setup.exe`` as an administrator
   (:ref:`Why? <faq_host_admin_privs>`)
2. You will be greeted by an intro screen. Press ``Next`` to continue.
3. You are presented with the |license| license. Please read and agree to the
   license by pressing ``Agree``.
4. You can change the install path if you wish, otherwise press ``Next`` to
   continue.
5. You may enable or disable options on this screen to configure the
   installation. The default values are recommended for most users.
   Press ``Install`` to begin installation.
6. After a few moments, installation will complete, and you will have a
   running instance of Looking Glass. If you experience failures, you can
   see them in the install log appearing in the middle of the window.
7. Press ``Close`` to exit the installer.

Command line users can run ``looking-glass-host-setup.exe /S`` to execute a
silent install with default options selected. Further configuration from the
command line can be done with flags. You can list all available flags by
running ``looking-glass-host-setup.exe /?``.
