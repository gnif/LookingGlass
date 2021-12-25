.. _building:

Building
########

The following instructions will help you build Looking Glass from source code.
Before attempting this, you should have a basic understanding of
how to use the shell.

.. _download_source:

Downloading
-----------

Visit the Looking Glass `Download Page <https://looking-glass.io/downloads>`__,
and download the stable version (**recommended**).
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

   When using the latest bleeding-edge client version,
   you *MUST* download and install the corresponding host application.

.. _build_client_section:

Client
------

.. _installing_build_dependencies:

Installing Build Dependencies
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

These required libraries and tools should be installed first.

.. _client_dependencies:

Required Dependencies
^^^^^^^^^^^^^^^^^^^^^

-  cmake
-  gcc, g++ \| clang
-  libegl-dev
-  libgl-dev
-  libgles-dev
-  libfontconfig-dev
-  libgmp-dev
-  libspice-protocol-dev
-  make
-  nettle-dev
-  pkg-config

.. _client_deps_may_be_disabled:

May be disabled
<<<<<<<<<<<<<<<

These dependencies are required by default, but may be omitted if their
feature is disabled when running :ref:`cmake <client_building>`.

-  Disable with ``cmake -DENABLE_BACKTRACE=no ..``

   -  binutils-dev

-  Disable with ``cmake -DENABLE_X11=no ..``

   -  libx11-dev
   -  libxfixes-dev
   -  libxi-dev
   -  libxinerama-dev
   -  libxss-dev
   -  libxcursor-dev
   -  libxpresent-dev

-  Disable with ``cmake -DENABLE_WAYLAND=no ..``

   -  libxkbcommon-dev
   -  libwayland-bin
   -  libwayland-dev
   -  wayland-protocols


.. _client_deps_recommended:

Recommended
<<<<<<<<<<<

-  fonts-dejavu-core (This is the default UI font, but a random font will
   be chosen if not available).

.. _client_fetching_with_apt:

Fetching with APT
^^^^^^^^^^^^^^^^^

You can fetch these dependencies with the following command:

``apt-get install binutils-dev cmake fonts-dejavu-core libfontconfig-dev
gcc g++ pkg-config libegl-dev libgl-dev libgles-dev libspice-protocol-dev
nettle-dev libx11-dev libxcursor-dev libxi-dev libxinerama-dev
libxpresent-dev libxss-dev libxkbcommon-dev libwayland-dev wayland-protocols``

You may omit some dependencies, if you disable the feature which requires them
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

This will build the **looking-glass-client** binary, which is used to display
frames from the guest.

.. note::

   For users running GNOME on Wayland, you likely want to pass
   ``-DENABLE_LIBDECOR=ON`` to ``cmake``, i.e. run ``cmake -DENABLE_LIBDECOR=ON ../``.

   For details, see :ref:`the FAQ <gnome_wayland_decorations>`.

.. seealso::

   -  :ref:`Installing the Client <client_install>`
   -  :ref:`Client Usage <client_usage>`

.. note::

   The most common compile error is related to backtrace support. This can be
   disabled by adding the following option to the cmake command:
   ``-DENABLE_BACKTRACE=0``. However, if you disable this and need support for
   a crash, use ``gdb`` to obtain a backtrace manually.

.. _host_building:

Host
----

These instructions help you build the host yourself from the
:ref:`downloaded source code <download_source>`.

.. warning::
   Building the host from source code is not recommended for most purposes,
   and should only be attempted by users who are prepared to handle issues
   on their own. Please download the pre-built binary installers from
   https://looking-glass.io/downloads for stability, and increased support.

.. note::
   The pre-built binaries also include NvFBC support built in, which is
   only available to current Nvidia SDK license holders, and cannot
   be enabled when building the host without also having a license.

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
