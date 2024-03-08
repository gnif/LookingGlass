.. _installing_client:

Client Application Installation
###############################

.. _client_install:

For Linux
---------

The Looking Glass client receives frames from the :ref:`host <host_install>` to
display on your screen. It also handles input, and can optionally share the
system clipboard with your guest OS through SPICE.

First you must build the client from source, see :ref:`building`. Once you have
built the client, you can install it. Run the following as root::

   make install

To install for the local user only, run::

   cmake -DCMAKE_INSTALL_PREFIX=~/.local .. && make install
