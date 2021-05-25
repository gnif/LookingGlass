OBS plugin
##########

You can add a Looking Glass video feed
to OBS as a video source with the included OBS plugin. This provides a
lower-latency alternative to capturing the Looking Glass client window
with a Screen or Window Capture source.

This may help improve your viewers' experience watching your stream, and
allows you to use your host without them seeing.

Build Instructions
~~~~~~~~~~~~~~~~~~

The OBS plugin is included in the main source tree of Looking Glass. The
building process is very similar to the
:ref:`client's <build_client_section>`.

Dependencies
^^^^^^^^^^^^

The OBS plugin requires the following extra dependencies alongside the
:ref:`client's build
dependencies <installing_build_dependencies>`.

-  libobs-dev

Please install this package or the equivalent in your package manager.

.. code:: bash

   apt-get install libobs-dev


.. _obs_building:

Building
^^^^^^^^

These instructions are the same as when building the
:ref:`client <client_building>`.

.. code:: bash

   mkdir obs/build
   cd obs/build
   cmake ../
   make

Installation
~~~~~~~~~~~~

The resulting liblooking-glass-obs.so file should be placed in your OBS
plugin directory.

.. code:: bash

   mkdir -p ~/.config/obs-studio/plugins/looking-glass-obs/bin/64bit
   cp liblooking-glass-obs.so ~/.config/obs-studio/plugins/looking-glass-obs/bin/64bit

Setup
~~~~~

Once installed, you can select the *"Looking Glass Client"* source from
the OBS sources menu. The configuration only requires the IVSHMEM file
that is used by the VM, and this is pre-populated with the default
filename for Looking Glass.

.. _open_broadcaster_software:


Open Broadcaster Software
~~~~~~~~~~~~~~~~~~~~~~~~~

The plugin is made for OBS, an open source streaming and recording
studio. Find out more at https://obsproject.com/

It's available under most distributions as *obs-studio* or just *obs*.

Screenshots
~~~~~~~~~~~

.. figure:: images/Looking-Glass-OBS-Source-Add.png
   :alt: Adding the plugin as a video source

   Adding the plugin as a video source

.. figure:: images/Looking-Glass-OBS-config.png
   :alt: Plugin configuration settings

   Plugin configuration settings
