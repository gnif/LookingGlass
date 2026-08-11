.. _idd_configuration:

Configure the IDD
#################

Right-click the **Looking Glass (IDD)** icon in the Windows notification area
and select **Open configuration**. The icon and its tooltip also show whether
the driver is using GPU acceleration or software processing.

Display modes
-------------

The mode list controls the resolutions and refresh rates that Windows may use
for the Looking Glass monitor.

* Select a mode to edit its width, height or refresh rate, then select
  **Update**.
* Select **<add new>**, enter a mode in the fields below the list and select
  **Update** to add it.
* Select **Delete** to remove the selected mode.
* Select **Load default** to replace the working list with the standard modes.
* Enable **prefer** on the mode that Windows should prefer. Only one mode can
  be preferred.

Refresh rates may contain up to three decimal places. For example, enter
``119.970`` rather than rounding it to 120 Hz. Accepted values range from
23.900 Hz to 1000.000 Hz. Width may range from 640 to 16384 pixels and height
from 480 to 16384 pixels.

Edits to the mode list are not applied immediately. Select **Save & reload
driver** when the list is ready. This saves the list, removes and recreates the
virtual monitor, and can make the display blink briefly. **Revert** discards
unsaved mode and default-refresh changes.

Default refresh
---------------

**Default refresh** is used when the client asks the IDD to create a dynamic
resolution. It also supplies the refresh rate when **Load default** rebuilds
the standard mode list.

Changing this value does not rewrite refresh rates already saved in the normal
mode list. On reload, an existing dynamic mode keeps its resolution and adopts
the new default refresh rate.

The client option ``win:setGuestRes`` enables automatic dynamic resolution
requests when the client window changes size. The default is enabled when the
producer supports it. Press the client's escape key together with ``=`` to
request the current window resolution manually.

Preferences
-----------

Make LG the only monitor
   Makes the Looking Glass display the only active Windows monitor. This is
   enabled by default and the helper restores the topology when required.
   Disable it if you intentionally use other guest displays at the same time.
   Disabling it stops future enforcement but does not automatically restore
   displays that Windows has already disabled.

Disable no GPU warning
   Suppresses the notification shown when the IDD has fallen back to software
   processing. It does not enable GPU acceleration or change the active
   adapter.

Unlike mode-list edits, preference checkboxes are saved when clicked.

Software processing
-------------------

If no suitable Windows render adapter is available, the IDD can use software
processing. This provides a display but is slower, cannot provide predictable
high-rate cadence and is limited to SDR. Check the IDD log to see which render
adapter was selected and whether software processing is active.

Modes that do not fit in shared memory
--------------------------------------

At startup the IDD removes modes that cannot fit in the configured IVSHMEM
region. If every suitable mode is filtered, the monitor cannot start. A
dynamic resolution request that is too large is refused and the helper reports
the minimum power-of-two IVSHMEM size needed.

Increase the IVSHMEM size in the VM configuration and restart the VM. The IDD
reads the new capacity when it starts; another helper reload is not normally
required. Allocating more shared memory than required does not improve
performance; it only reserves additional host RAM.
