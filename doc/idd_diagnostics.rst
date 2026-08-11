.. _idd_diagnostics:

IDD status and logs
###################

The IDD helper in the Windows notification area is the first place to check
when the virtual display is missing or slow. Its icon and tooltip report
whether the driver is using GPU acceleration or software processing.

Right-click the helper and open the log directory. The files are stored in:

``C:\ProgramData\Looking Glass (IDD)``

Collect these files when reporting an IDD fault:

* ``looking-glass-idd.txt`` -- display creation, GPU selection, modes,
  transport and frame scheduling;
* ``looking-glass-input.txt`` -- direct keyboard and mouse device activity;
* ``looking-glass-idd-service.txt`` -- service and driver control requests;
* ``looking-glass-idd-helper.txt`` -- configuration, topology and user
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
   available. Their absence on Windows 10 is expected and does not prevent SDR
   use.

Frame schedule owner
   Reports the fastest active client's requested rate, guest acquisition rate
   and how many frames were published or skipped. Skipping excess guest frames
   is expected; it saves memory bandwidth while preserving the newest frame
   needed by the client.

Input owner
   Identifies the client currently allowed to send direct input. Only one
   client can own it at a time.

Client information
------------------

Run the client from a terminal and retain its complete output. Include:

* the client, IDD and OBS versions;
* Linux distribution and kernel;
* X11 or Wayland and the compositor name;
* host and guest GPU models and drivers;
* whether ``lgmp:allowDMA`` is enabled;
* the guest resolution and refresh rate; and
* exact steps that reproduce the fault.

Do not copy only the final error line. Startup output records the selected
transport, renderer, display server, audio backend and import method, which are
often needed to explain it.
