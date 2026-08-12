.. _building:

Build the Linux client
######################

The Looking Glass Client is currently distributed as source code. Building it
is a normal part of installation, not an optional developer step. These
instructions require basic familiarity with a Linux shell.

.. _download_source:

Downloading
-----------

Visit the Looking Glass `Download Page <https://looking-glass.io/downloads>`__,
and download the stable version (**Recommended**).
You can also download a *bleeding-edge version*, or the latest RC version
during a Release Candidate cycle.

Developers can clone the source code repo with ``git``.

.. code:: bash

   git clone --recursive https://github.com/gnif/LookingGlass.git

.. warning::

   Please only clone from Git if you're a developer, and know what you're
   doing. Looking Glass requires git submodules that must be setup and updated
   when building. Source code downloads from the website come bundled with the
   necessary submodules.

.. note::

   The current client, IDD and OBS plugin must come from the same Looking Glass
   release. Bleeding-edge builds must be paired with their matching
   bleeding-edge components. Legacy Host users must instead use the complete
   matching B7 stack described in :ref:`legacy_host_policy`.

.. _build_client_section:

Client Application
------------------

.. _installing_build_dependencies:

Installing build dependencies
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

These required libraries and tools should be installed first.

.. note::

   The below list of dependencies is for Debian. A community maintained list of
   dependencies for other distributions for the current **stable** version of
   Looking Glass is maintained on the wiki at:
   https://looking-glass.io/wiki/Installation_on_other_distributions

.. _client_dependencies:

Required dependencies
^^^^^^^^^^^^^^^^^^^^^

..
   Editor note: Listed dependencies are Debian packages containing the
   required resources. All dependencies must be explicitly defined.
   Omitting required dependencies that happen to be pulled in via
   Depends: or Recommends: from another listed package is not allowed.
   All required packages must be listed.

-  ``cmake``
-  ``binutils``
-  ``gcc``, ``g++`` \| ``clang``
-  ``libegl-dev``
-  ``libgl-dev``
-  ``libgles-dev``
-  ``libfontconfig-dev``
-  ``libgmp-dev``
-  ``libspice-protocol-dev``
-  ``libxkbcommon-dev``
-  ``make``
-  ``nettle-dev``
-  ``pkg-config``

.. _client_deps_may_be_disabled:

May be disabled
<<<<<<<<<<<<<<<

These dependencies are required by default, but may be omitted if their
feature is disabled when running :ref:`cmake <client_building>`.

-  Disable with ``cmake -DENABLE_BACKTRACE=no ..``

   -  ``libdw-dev``
   -  ``libunwind-dev``

-  Disable with ``cmake -DENABLE_X11=no ..``

   -  ``libx11-dev``
   -  ``libxfixes-dev``
   -  ``libxi-dev``
   -  ``libxinerama-dev``
   -  ``libxss-dev``
   -  ``libxcursor-dev``
   -  ``libxpresent-dev``
   -  ``libxrandr-dev``

-  Disable with ``cmake -DENABLE_WAYLAND=no ..``

   -  ``libwayland-bin``
   -  ``libwayland-dev``

-  Disable all audio support with ``cmake -DENABLE_AUDIO=no ..``

   -  ``libpipewire-0.3-dev``
   -  ``libpulse-dev``
   -  ``libsamplerate0-dev``
   -  ``libusbredirparser-dev``

-  Disable with ``cmake -DENABLE_PIPEWIRE=no ..``

   -  ``libpipewire-0.3-dev``

-  Disable with ``cmake -DENABLE_PULSEAUDIO=no ..``

   -  ``libpulse-dev``

-  Disable USB audio with ``cmake -DENABLE_USB_AUDIO=no ..``

   -  ``libusbredirparser-dev`` version 0.7.1 or newer

``libsamplerate0-dev`` is required whenever audio support remains enabled.

.. _client_deps_recommended:

Recommended
<<<<<<<<<<<

-  ``fonts-dejavu-core`` (This is the default UI font, but a random font will
   be chosen if not available).

.. _client_fetching_with_apt:

Fetching with APT
^^^^^^^^^^^^^^^^^

You can fetch these dependencies with the following command:

.. warning::

   The command below builds both PipeWire and PulseAudio playback backends.
   Omit one development package only when also disabling its backend in CMake.
   Microphone recording requires PipeWire; the current PulseAudio backend is
   playback-only.

.. code:: bash

   apt-get install binutils cmake make fonts-dejavu-core libdw-dev \
   libfontconfig-dev libgmp-dev libunwind-dev gcc g++ pkg-config \
   libegl-dev libgl-dev libgles-dev libspice-protocol-dev nettle-dev \
   libx11-dev libxcursor-dev libxfixes-dev libxi-dev libxinerama-dev \
   libxpresent-dev libxrandr-dev libxss-dev libxkbcommon-dev \
   libwayland-bin libwayland-dev \
   libpipewire-0.3-dev libpulse-dev libsamplerate0-dev \
   libusbredirparser-dev

You may omit some dependencies if you disable the feature which requires them
when running :ref:`cmake <client_building>`.
(See :ref:`client_deps_may_be_disabled`)

.. _client_building:

Building
~~~~~~~~

If you've downloaded the source code as a zip file, simply unzip and ``cd``
into the new directory. If you've cloned the repo with ``git``, then ``cd``
into the *LookingGlass* directory.

.. code:: bash

   mkdir client/build
   cd client/build
   cmake ../
   make

This will build the ``looking-glass-client`` binary, which is used to display
frames from the guest. It also produces ``looking-glass-client.debug``. Keep
this file with the matching binary to obtain source locations in crash stack
traces. The install target places it in the standard ``bin/.debug`` location
automatically.

You can then :ref:`continue installing Looking Glass <client_install>`, or run
it directly from the build directory:

.. code:: bash

   ./looking-glass-client

.. seealso::

   -  :ref:`Client installation <client_install>`
   -  :ref:`Client usage <client_usage>`

.. note::

   For users running GNOME on Wayland, you may want to enable ``libdecor`` when
   building.

   .. code:: bash

      cmake -DENABLE_LIBDECOR=ON ../

   For details, see :ref:`the FAQ <gnome_wayland_decorations>`.

.. note::

   The most common compile error is related to backtrace support. Try disabling
   this when building:

   .. code:: bash

      cmake -DENABLE_BACKTRACE=0 ../

   If you disable this and need support for a crash, use ``gdb`` to obtain a
   backtrace manually.

.. _host_building:

Legacy Host Application build
-----------------------------

.. warning::

   The Host Application is a legacy server application. New installations should
   use the prebuilt :doc:`Looking Glass IDD <install_idd>`. These build
   instructions are retained for the complete matching B7 stack described in
   :ref:`legacy_host_policy`.

.. note::

   `Host` in this context refers to the `Looking Glass (LG) host server application`,
   to which the LG client connects to, it thus does not refer to the `host OS`!

   A common setup is to have a `host OS` running a `guest VM`. In such a setup
   the `client application` runs on the `host OS`; and the `host application`
   on the `guest VM`.

   The term `Host application` was chosen over `Guest application`, because LG
   can be run in a VM to VM configuration, in which case both Host Application and
   Client Application are run inside VM Guests.

These instructions help you build the host yourself from the
:ref:`downloaded source code <download_source>`.

.. warning::
   :name: dont-build-the-host

   Building the host from source code is not recommended for most purposes,
   and should only be attempted by users who are prepared to handle issues
   on their own. Please download the pre-built binary installers from
   https://looking-glass.io/downloads for stability, and increased support.

   .. note::

      The pre-built binaries also include NvFBC support built in, which is
      only available to current Nvidia SDK license holders, and cannot
      be enabled when building the host without also having a license.

   (`link <#dont-build-the-host>`_)

.. _host_win_on_win:

For Windows on Windows
~~~~~~~~~~~~~~~~~~~~~~

1. Download and install msys2 x86_64 from
   `http://www.msys2.org/ <http://www.msys2.org/>`__ following the setup
   instructions provided

2. Run the MSYS2 shell

3. Download build dependencies with pacman

.. code:: bash

   pacman -Fy
   pacman -Sy git make mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake

4. Change directory to the source tree with ``cd``

5. Configure the project and build it

.. code:: bash

   mkdir host/build
   cd host/build
   cmake -G "MSYS Makefiles" ..
   make

.. _host_linux_on_linux:

For Linux on Linux
~~~~~~~~~~~~~~~~~~

Make a ``host/build`` directory, then run ``cmake``

.. code:: bash

   mkdir host/build
   cd host/build
   cmake ..
   make

.. _host_win_cross_on_linux:

For Windows cross compiling on Linux
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Like :ref:`host_linux_on_linux`, but using the mingw64 toolchain to
cross-compile a Windows ``.exe`` file.

.. code:: bash

   mkdir host/build
   cd host/build
   cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw64.cmake ..
   make

.. _host_build_installer:

Building the Windows installer
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

1. :ref:`Build <host_win_cross_on_linux>` the host on Linux.

2. Install ``nsis``

.. code:: bash

   apt-get install nsis

3. Use ``makensis`` to build the installer.

.. code:: bash

   cd host/build/platform/Windows
   makensis installer.nsi

.. _host_questions:

This will build ``looking-glass-host-setup.exe`` under
``host/build/platform/Windows/looking-glass-host-setup.exe``

.. seealso::

   :ref:`Installing the Host <host_install>`
