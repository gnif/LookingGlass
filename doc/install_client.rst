.. _installing_client:

Client Application Installation
###############################

.. _client_install:

For Linux
---------

The Looking Glass Client receives frames from the Windows producer and displays
them on Linux. It also handles input, audio, overlays and optional SPICE
services.

There are currently no Linux distribution packages for the client. First
:ref:`build it from source <build_client_section>`, then run the following from
the client build directory as root::

   make install

To install for the current user only, configure the build with a user-local
prefix before installing::

   cmake -DCMAKE_INSTALL_PREFIX="$HOME/.local" ..
   make install

Ensure ``$HOME/.local/bin`` is in your ``PATH`` when using the user-local
installation.
