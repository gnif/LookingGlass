Installation
############

.. _looking_glass_client:

Looking Glass Client
--------------------

This guide will step you through building the looking glass client from
source, before you attempt to do this you should have a basic
understanding of how to use the shell.

.. _building_the_application:

Building the Application
~~~~~~~~~~~~~~~~~~~~~~~~

.. _installing_build_dependencies:

Installing Build Dependencies
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

These required libraries and tools should be installed first.

.. _required_dependencies:

Required Dependencies
'''''''''''''''''''''

-  cmake
-  gcc \| clang
-  fonts-freefont-ttf
-  libegl-dev
-  libgl-dev
-  libfontconfig1-dev
-  libgmp-dev
-  libsdl2-dev
-  libsdl2-ttf-dev
-  libspice-protocol-dev
-  make
-  nettle-dev
-  pkg-config

.. _may_be_disabled:

May be disabled
<<<<<<<<<<<<<<<

These dependencies are required by default, but may be omitted if their
feature is disabled when running :ref:`cmake <client_building>`.

-  Disable with ``cmake -DENABLE_BACKTRACE=no``

   -  binutils-dev

-  Disable with ``cmake -DENABLE_X11=no``

   -  libx11-dev
   -  libxfixes-dev
   -  libxi-dev
   -  libxss-dev

-  Disable with ``cmake -DENABLE_WAYLAND=no``

   -  libwayland-bin
   -  libwayland-dev
   -  wayland-protocols

You can fetch these dependencies on Debian systems with the following command:

``apt-get install binutils-dev cmake fonts-freefont-ttf libfontconfig1-dev
libsdl2-dev libsdl2-ttf-dev libspice-protocol-dev libx11-dev nettle-dev
wayland-protocols``

Downloading
^^^^^^^^^^^

Either visit the Looking Glass website's `Download
Page <https://looking-glass.io/downloads>`_, or pull the lastest **bleeding-edge
version** with ``git``.

.. code:: bash

   git clone --recursive https://github.com/gnif/LookingGlass.git

.. note::

   When using the latest bleeding-edge client version,
   you *MUST* download and install the corresponding host application.

.. _client_building:

Building
^^^^^^^^

If you've downloaded the source code as a zip file, simply unzip and cd into the
new directory. If you've cloned the repo with ``git``, then ``cd`` into the
'LookingGlass' directory.

.. code:: bash

   mkdir client/build
   cd client/build
   cmake ../
   make

.. note::

   The most common compile error is related to backtrace support. This can be
   disabled by adding the following option to the cmake command:
   **-DENABLE_BACKTRACE=0**, however, if you disable this and need support for a
   crash please be sure to use gdb to obtain a backtrace manually or there is
   nothing that can be done to help you.

Should this all go well, you will build the **looking-glass-client**.
Before you run the client, you will first need
to configure either libvirt, or QEMU (whichever you prefer) then set
up the **looking-glass-host** service in your VM.

You can run the client from the build directory, add the build directory to your
path to call it from anywhere, or run this command

.. code:: bash

   ln -s $(pwd)/looking-glass-client /usr/local/bin/

from the build directory.

.. _libvirt_configuration:

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
adjusted as per the :ref:`Determining Memory <determining_memory>` section.

.. _spice_server:

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
:ref:`how_to_enable_clipboard_synchronization_via_spice`

AppArmor
^^^^^^^^

For libvirt versions before **5.10.0**, if you are using AppArmor, you
need to add permissions for QEMU to access the shared memory file. This
can be done by adding the following to
*/etc/apparmor.d/local/abstractions/libvirt-qemu*.

``/dev/shm/looking-glass rw,``

.. _qemu_commands:

Qemu Commands
~~~~~~~~~~~~~

**If you are using libvirt/virt-manager, then this does not apply to you.**

Add the following to the commands to your QEMU command line, adjusting
the ``bus`` parameter to suit your particular configuration:

.. code:: bash

   -device ivshmem-plain,memdev=ivshmem,bus=pcie.0 \
   -object memory-backend-file,id=ivshmem,share=on,mem-path=/dev/shm/looking-glass,size=32M

The memory size (shown as 32M in the example above) may need to be
adjusted as per :ref:`Determining Memory <determining_memory>` section.

.. _determining_memory:

Determining Memory
~~~~~~~~~~~~~~~~~~

You will need to adjust the memory size to be suitable for
your desired maximum resolution, with the following formula:

``width x height x 4 x 2 = total bytes``

``total bytes / 1024 / 1024 = total megabytes + 10``

For example, for a resolution of 1920x1080 (1080p):

``1920 x 1080 x 4 x 2 = 16,588,800 bytes``

``16,588,800 / 1024 / 1024 = 15.82 MB + 10 = 25.82``

You must round this value up to the nearest power of two, which for the
provided example is 32MB.

.. _shared_memory_file_permissions:

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

.. _looking_glass_service_windows:

Looking Glass Service (Windows)
-------------------------------

You must first run the Windows VM with the changes noted above in either
the :ref:`libvirt_configuration` or :ref:`qemu_commands` sections.

.. _installing_the_ivshmem_driver:

Installing the IVSHMEM Driver
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

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
is turned off in the bios/uefi settings of your virtual machine.

.. _a_note_about_ivshmem_and_scream_audio:

A note about IVSHMEM and Scream Audio
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. warning::
   Using IVSHMEM with Scream may interfere with Looking Glass, as they may try
   to use the same device.

Please do not use the IVSHMEM plugin for Scream.
Use the default network transfer method. The IVSHMEM method induces
additional latency that is built into its implementation. When using
VirtIO for a network device the VM is already using a highly optimized
memory copy anyway so there is no need to make another one.

If you insist on using IVSHMEM for Scream—despite its inferiority to the
default network implementation—the Windows Host Application can be told
what device to use. Create a ``looking-glass-host.ini`` file in the same
directory as the looking-glass-host.exe file. In it, you can use the
``os:shmDevice`` option like so:

.. code:: INI

   [os]
   shmDevice=1

.. _using_the_windows_host_application:

Using the Windows Host Application
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Start by downloading the correct version for your release from
https://looking-glass.io/downloads. You can either choose between
**Official Releases**, which are stable; or **Release Candidates**, new versions
about to be stable, but haven't passed validation.

.. note::
   If your **looking-glass-client** was created by building from the **master
   branch** you have to pick the **Bleeding Edge** version.

Next, extract the zip archive using the commit hash for the password.
Then, run the ``looking-glass-host-setup.exe`` installer and install the host.
By default, the installer will install a service that
automatically starts the host application at boot. The installer can
also be installed in silent mode with the ``/S`` switch. You can find other
command line options with the ``/h`` switch.

The windows host application captures the windows desktop and stuffs the
frames into the shared memory via the shared memory virtual device,
without this Looking Glass will not function. It is critical that the
version of the host application matches the version of the client
application, as differing versions can be, and usually are,
incompatible.

.. note::
   As of 2020-10-23, Microsoft Defender is known to mark the
   Looking-Glass host executable as a virus and in some cases will
   automatically delete the file.

.. _running_the_client:

Running the Client
------------------

The client command is the binary file: **looking-glass-client**. This
command should run after the Windows Host Application has started.

.. _client_key_bindings:

Default Key Bindings
~~~~~~~~~~~~~~~~~~~~

By default Looking Glass uses the :kbd:`Scroll Lock` key as the escape key
for commands as well as the input :kbd:`capture` mode toggle, this can be
changed using the ``-m`` switch if you desire a different key. Below are
a list of current key bindings:

============================ =======================================================
Command                      Description
============================ =======================================================
:kbd:`ScrLk`                 Toggle capture mode
:kbd:`ScrLk` + :kbd:`Q`      Quit
:kbd:`ScrLk` + :kbd:`R`      Rotate the output clockwise by 90° increments
:kbd:`ScrLk` + :kbd:`I`      Spice keyboard & mouse enable toggle
:kbd:`ScrLk` + :kbd:`S`      Toggle scale algorithm
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

Options may be provided with a short form, if available, or directly by
long form. Boolean options may be specified without a paramater to toggle their
state.

Examples:

- ``looking-glass-client -F`` (short)
- ``looking-glass-client win:fullScreen`` (long)
- ``looking-glass-client -f /dev/shm/my-lg-shmem`` (short with parameter)
- ``looking-glass-client app:shmFile=/dev/shm/my-lg-shmem`` (long with parameter)

.. _client_config_options_file:

Configuration Files
~~~~~~~~~~~~~~~~~~~

By default the application will look for and load the config files in
the following locations

-  /etc/looking-glass-client.ini
-  ~/.looking-glass-client.ini

The format of this file is the commonly known INI format, for example::

   [win]
   fullScreen=yes

   [egl]
   nvGain=1

Command line arguments will override any options loaded from the config
files.


.. _client_full_command_options:

Full Command Line Options
~~~~~~~~~~~~~~~~~~~~~~~~~

The following is a complete list of options accepted by this application

  +------------------------+-------+------------------------+----------------------------------------------------------------------------------------+
  | Long                   | Short | Value                  | Description                                                                            |
  +========================+=======+========================+========================================================================================+
  | app:configFile         | -C    | NULL                   | A file to read additional configuration from                                           |
  +------------------------+-------+------------------------+----------------------------------------------------------------------------------------+
  | app:renderer           | -g    | auto                   | Specify the renderer to use                                                            |
  +------------------------+-------+------------------------+----------------------------------------------------------------------------------------+
  | app:license            | -l    | no                     | Show the license for this application and then terminate                               |
  +------------------------+-------+------------------------+----------------------------------------------------------------------------------------+
  | app:cursorPollInterval |       | 1000                   | How often to check for a cursor update in microseconds                                 |
  +------------------------+-------+------------------------+----------------------------------------------------------------------------------------+
  | app:framePollInterval  |       | 1000                   | How often to check for a frame update in microseconds                                  |
  +------------------------+-------+------------------------+----------------------------------------------------------------------------------------+
  | app:allowDMA           |       | yes                    | Allow direct DMA transfers if supported (see `README.md` in the `module` dir)          |
  +------------------------+-------+------------------------+----------------------------------------------------------------------------------------+
  | app:shmFile            | -f    | /dev/shm/looking-glass | The path to the shared memory file, or the name of the kvmfr device to use, ie: kvmfr0 |
  +------------------------+-------+------------------------+----------------------------------------------------------------------------------------+

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
  | win:minimizeOnFocusLoss |       | yes                    | Minimize window on focus loss                                        |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:fpsMin              | -K    | -1                     | Frame rate minimum (0 = disable - not recommended, -1 = auto detect) |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:showFPS             | -k    | no                     | Enable the FPS & UPS display                                         |
  +-------------------------+-------+------------------------+----------------------------------------------------------------------+
  | win:ignoreQuit          | -Q    | no                     | Ignore requests to quit (ie: Alt+F4)                                 |
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

  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | Long                         | Short | Value               | Description                                                                      |
  +==============================+=======+=====================+==================================================================================+
  | input:grabKeyboard           | -G    | yes                 | Grab the keyboard in capture mode                                                |
  +------------------------------+-------+---------------------+----------------------------------------------------------------------------------+
  | input:grabKeyboardOnFocus    |       | yes                 | Grab the keyboard when focused                                                   |
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
  | spice:clipboard        |       | yes       | Use SPICE to syncronize the clipboard contents with the guest       |
  +------------------------+-------+-----------+---------------------------------------------------------------------+
  | spice:clipboardToVM    |       | yes       | Allow the clipboard to be syncronized TO the VM                     |
  +------------------------+-------+-----------+---------------------------------------------------------------------+
  | spice:clipboardToLocal |       | yes       | Allow the clipboard to be syncronized FROM the VM                   |
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

  +---------------------+-------+-------+-----------------------+
  | Long                | Short | Value | Description           |
  +=====================+=======+=======+=======================+
  | wayland:warpSupport |       | yes   | Enable cursor warping |
  +---------------------+-------+-------+-----------------------+
