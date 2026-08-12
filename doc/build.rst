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
