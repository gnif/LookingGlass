.. _host_usage:

Legacy Host configuration
#########################

.. warning::

   This page applies only to the legacy Windows Host Application. New
   installations should use the IDD. B7 is the last recommended complete stack
   when non-capture mouse input through the Host Application is required.

The legacy Host normally selects a compatible frame path automatically. Its
persistent configuration file is:

``C:\Program Files\Looking Glass (host)\looking-glass-host.ini``

Use options from the same release as the Host binary. Do not copy a current
Host configuration into B7 or combine a B7 Host with a current client.

.. _host_capture:

Capture interface
-----------------

The legacy Host captures an existing Windows display through Desktop
Duplication or NvFBC. It does not use Windows Graphics Capture. Select an
interface only when automatic selection fails:

.. code-block:: ini

   [app]
   capture=d12

.. _host_capture_d12:

D12
   The preferred Direct3D 12 Desktop Duplication path on supported systems. It
   can copy directly toward shared memory and supports damage tracking.

.. _host_capture_dxgi:

DXGI
   The Direct3D 11 Desktop Duplication compatibility path. It may require an
   additional staging copy and can show cursor-related capture jitter.

.. _host_capture_nvfbc:

NvFBC
   An NVIDIA SDK path available only with supported hardware and licensing.
   It is not required by the IDD or the normal legacy D12 path.

The exact capture-interface options can change between releases. Consult the
binary's help and the configuration comments supplied with that release.

.. _host_select_ivshmem:

Select an IVSHMEM device
------------------------

The legacy Host selects the first IVSHMEM device by default. When the VM has
more than one, ``os:shmDevice`` selects by zero-based device order:

.. code-block:: ini

   [os]
   shmDevice=1

Check the Host log after changing it; the selected device is marked with an
asterisk. This option is for the legacy Host and is not an IDD helper setting.

.. _host_downsampling:

Downsampling
------------

The legacy Host can reduce a captured resolution before transport. Rules use
``source:target`` syntax and may match resolutions greater than a threshold:

.. code-block:: ini

   ; Downsample exactly 3840x2160 to 1920x1080
   downsample=3840x2160:1920x1080

   ; Downsample anything larger than 1920x1080
   downsample=>1920x1080:1920x1080

This saves transport bandwidth but does not reduce the guest application's
rendering work. IDD users should request the required virtual monitor
resolution instead.
