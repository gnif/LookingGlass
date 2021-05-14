Looking Glass Host
##################

.. _host_building:

Building
--------

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
   ``git clone https://github.com/gnif/LookingGlass.git``
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

Building the Windows installer
------------------------------

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

Questions and Answers
---------------------

Where is the log?
~~~~~~~~~~~~~~~~~

The log file for the host application is located at::

   %ProgramData%\Looking Glass (host)\looking-glass-host.txt

You can also find out where the file is by right clicking on the tray
icon and selecting "Log File Location".

The log file for the looking glass service is located at::

   %ProgramData%\Looking Glass (host)\looking-glass-host-service.txt

This is useful for troubleshooting errors related to the host
application not starting.

High priority capture using DXGI and Secure Desktop (UAC) capture support
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

By default Windows gives priority to the foreground application for any
GPU work which causes issues with capture if the foreground application
is consuming 100% of the available GPU resources. The looking glass host
application is able to increase the kernel GPU thread to realtime
priority which fixes this, but in order to do so it must run as the
``SYSTEM`` user account. To do this, Looking Glass needs to run as a
service. This can be accomplished by either using the NSIS installer
which will do this for you, or you can use the following command to
Install the service manually:

::

   looking-glass-host.exe InstallService

To remove the service use the following command:

::

   looking-glass-host.exe UninstallService

This will also enable the host application to capture the secure desktop
which includes things like the lock screen and UAC prompts.

Why does this version require Administrator privileges?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

This is intentional for several reasons.

1. NvFBC requires a system wide hook to correctly obtain the cursor
   position as NVIDIA decided to not provide this as part of the cursor
   updates.
2. NvFBC requires administrator level access to enable the interface in
   the first place. (WIP)
3. DXGI performance can be improved if we have this. (WIP)

NvFBC (NVIDIA Frame Buffer Capture)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Why can't I compile NvFBC support into the host?
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

You must download and install the NVidia Capture SDK. Please note that
by doing so you will be agreeing to NVIDIA's SDK License agreement.

*-Geoff*
 
