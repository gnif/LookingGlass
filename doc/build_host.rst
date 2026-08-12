.. _host_building:

Legacy Host Application build
#############################

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
