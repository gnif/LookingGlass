.. _idd_diagnostics:

IDD status and logs
###################

The IDD helper in the Windows notification area is the first place to check
when the virtual display is missing or slow. Its icon and tooltip report
whether the driver is using GPU acceleration or software processing.

Right-click the helper and open the log directory. In case this is not
available, manually navigate to ``C:\ProgramData\Looking Glass (IDD)``.

Collect these files when reporting an IDD fault:

* ``looking-glass-idd.txt``: display creation, GPU selection, modes,
  transport and frame scheduling;
* ``looking-glass-input.txt``: direct keyboard and mouse device activity;
* ``looking-glass-idd-service.txt``: service and driver control requests; and
* ``looking-glass-idd-helper.txt``: configuration, topology and user
  notifications.

Each log rotates through suffixes ``.1`` to ``.4``. Include the current file
and rotated files when the problem happened before the most recent restart.

Useful IDD lines
----------------

Render adapter
   Shows the Windows GPU selected by the IDD. A software-processing warning
   explains why HDR, cadence and high frame rates are unavailable.

IVSHMEM size
   Confirms which device was opened and its capacity. Compare this with the VM
   configuration and :ref:`libvirt_determining_memory`.

Filtered mode
   A configured mode was omitted because its frame buffers do not fit. Increase
   IVSHMEM or remove the oversized mode.

IddCx capabilities
   Shows whether the runtime HDR and wide-color-gamut interfaces are
   available. This will not be visible on Windows 10, and SDR will be used
   instead.

Frame schedule owner
   Reports the fastest active client's requested rate, guest acquisition rate,
   and how many frames were published or skipped. Skipping excess guest frames
   is expected; it saves memory bandwidth while preserving the newest frame
   needed by the client.

Input owner
   Identifies the client currently allowed to send direct input. Only one
   client can own it at a time.
