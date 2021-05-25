.. _building:

Building
########

The following instructions will help you build Looking Glass for yourself
from source code. Before you attempt to do this, you should have a basic
understanding of how to use the shell.

.. _download_source:

Downloading
-----------

Either visit the Looking Glass website's `Download
Page <https://looking-glass.io/downloads>`_, or pull the lastest **bleeding-edge
version** with ``git``.

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


.. _client_building:

Building
~~~~~~~~

If you've downloaded the source code as a zip file, simply unzip and cd into the
new directory. If you've cloned the repo with ``git``, then ``cd`` into the
'LookingGlass' directory.

.. code:: bash

   mkdir client/build
   cd client/build
   cmake ../
   make

Should this all go well, you will build the **looking-glass-client**.

.. seealso::

   :ref:`Installing the Client <client_install>`

.. note::

   The most common compile error is related to backtrace support. This can be
   disabled by adding the following option to the cmake command:
   **-DENABLE_BACKTRACE=0**, however, if you disable this and need support for a
   crash please be sure to use gdb to obtain a backtrace manually or there is
   nothing that can be done to help you.

.. _host_building:

Host
----

These instructions help you build the host yourself from the :ref:`downloaded
source code <download_source>`.

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
2. Run the MSYS2 shell.
3. Download build dependencies with pacman

.. code:: bash

   pacman -Fy
   pacman -Sy git make mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake

4. Checkout the project

.. code:: bash

   git clone https://github.com/gnif/LookingGlass.git

5. Configure the project and build it

.. code:: bash

   mkdir LookingGlass/host/build
   cd LookingGlass/host/build
   cmake -G "MSYS Makefiles" ..
   make

.. _host_linux_on_linux:
   
For Linux on Linux
~~~~~~~~~~~~~~~~~~

Make a ``host/build`` direstory, then run ``cmake``

.. code:: bash

   cd host
   mkdir build
   cd build
   cmake ..
   make

.. _host_win_cross_on_linux:

For Windows cross compiling on Linux
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Like :ref:`host_linux_on_linux`, but specifying the mingw64 toolchain in cmake
for building.

.. code:: bash

   cd host
   mkdir build
   cd build
   cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw64.cmake ..
   make

.. _host_build_installer:

Building the Windows installer
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

1. :ref:`Build <host_win_cross_on_linux>` the host for Linux.

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
