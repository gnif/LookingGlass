.. _client_performance:

Measure performance and latency
###############################

Press :kbd:`ScrLk` + :kbd:`D` for the compact FPS and UPS display. Press
:kbd:`ScrLk` + :kbd:`T` for the timing graphs.

FPS and UPS
-----------

FPS
   How often the client completed a render and swap. Cursor-only and overlay
   redraws can raise this without a new desktop frame.

UPS
   How often this client consumed a new guest frame. It can be lower than the
   guest render rate because cadence deliberately skips frames that were
   superseded before the client's next deadline.

A higher number is not automatically lower latency. Use the stage graph to
find where time is spent and confirm that the displayed rate meets the output
you are actually using.

Frame latency graphs
--------------------

The **Minimum**, **Maximum** and **Average** panels show a running history in
200 ms buckets. Each colored band is a sequential stage. The thickness of a
band is its duration; its height above zero includes all earlier bands. The top
of the final **Swap** band is the plotted total through return from EGL swap.

The current incomplete bucket is not plotted. This keeps the newest point from
jumping as samples arrive. Each panel also keeps a stable vertical scale with
hysteresis, so read the millisecond axis when comparing panels.

.. list-table:: Producer and transport stages
   :widths: 18 82
   :header-rows: 1

   * - Band
     - What it measures
   * - **Capture**
     - Time spent successfully acquiring the next IDD swap-chain buffer. This
       is not Windows Graphics Capture and does not include guest application
       rendering or the wait between unsuccessful acquisition attempts.
   * - **Post**
     - IDD preparation before the measured copy, including format and damage
       processing, synchronization and any required compute effect.
   * - **Copy**
     - IDD work that copies the selected frame into retained or transport
       storage. It may include more than one copy when the selected path needs
       it.
   * - **Ready**
     - Remaining IDD queue, fence, finalization and publication overhead not
       included in Post, Copy or Hold.
   * - **Hold**
     - Time a prepared hardware frame waits for the cadence deadline before
       its final transport copy starts. This is intentional scheduling time.
       Software processing does not use cadence and reports zero.
   * - **Transport**
     - Inferred publication-to-client delay for a frame matched to a cadence
       deadline. It is not an “IVSHMEM copy” measurement and is omitted when
       producer and client timing cannot be matched safely.

.. list-table:: Client EGL stages
   :widths: 18 82
   :header-rows: 1

   * - Band
     - What it measures
   * - **Import**
     - Work needed to make the frame usable by EGL, such as a CPU staging copy
       or DMA import and snapshot submission.
   * - **Dispatch**
     - Remaining frame-thread validation, format, damage and queueing work
       after Import.
   * - **Queue**
     - Time from queueing the accepted update until the render thread begins.
       This includes waiting for display cadence and deliberate JIT slack.
   * - **Prepare**
     - Client render-thread preparation such as commands, resize handling,
       geometry and invalidation decisions.
   * - **Setup**
     - EGL state, damage history, HDR and pointer setup before desktop drawing.
   * - **Effects**
     - Evaluation of the active EGL filter chain.
   * - **Desktop**
     - Completion of the frame texture and drawing the guest desktop, with
       Effects shown separately.
   * - **Compose**
     - Cursor, letterbox, damage diagnostics and HDR composition around the
       desktop. The interactive UI overlay is deliberately excluded.
   * - **Swap**
     - Time inside the display-server EGL swap call. It may include blocking
       in EGL or compositor submission, but not later physical scanout.

Only guest frames actually consumed by EGL contribute producer samples.
Superseded frames are not treated as latency samples. The minimum and maximum
for each band are calculated independently, so the top of a minimum or maximum
stack can combine stages from different frames. Use those panels to locate
stage spikes, not as the measured total of one specific frame.

Cadence and Hold
----------------

The Linux display and Windows guest have independent clocks. Looking Glass
does not subtract their raw timestamps. The producer reports durations, while
periods, generations and deadlines identify a matching cadence event. The
Transport band is left absent when that match is not valid.

At equal nominal rates, a **Hold** sawtooth is normally a beat pattern between
two free-running refresh cycles. The age of the newest guest frame ramps as
their phase moves, then wraps when a newer frame reaches the next client
deadline. This is not accumulated cross-VM clock error.

Configure the IDD with the physical display's exact refresh rate to minimize
this pattern. For example, use 119.970 Hz when that is the rate reported for
the display instead of rounding it to 120 Hz.

Investigate when Hold repeatedly exceeds roughly one guest frame period, or
when jumps occur with large Post, Copy or Ready spikes. Those patterns can
indicate producer work or scheduling delays rather than normal phase drift.

Frame and photon summaries
--------------------------

The compact **FRAME** plot is the interval between completed client
render-and-swap loops. It includes cadence waits and cursor-only or overlay-only
redraws, so it is not the CPU time spent rendering one frame.

On Wayland compositors that support ``wp_presentation``, **PHOTON** measures
from immediately before the EGL swap request until the compositor reports
presentation. It includes Swap and must not be added to the stacked total. The
displayed reciprocal “Hz” describes average latency, not the actual monitor
presentation rate.

Finding a bottleneck
--------------------

* High **Copy** points to guest GPU or system-memory bandwidth and the IDD copy
  path.
* High **Hold** alone usually reflects cadence phase; correlate it with other
  stages before changing anything.
* High **Import** with DMA disabled points to the client-side memory copy.
* High **Queue** can be intentional cadence or JIT waiting. Disable
  ``win:jitRender`` while diagnosing unstable timing.
* High **Effects**, **Desktop** or **Compose** points to client GPU rendering.
* A stable high **Swap** or **PHOTON** value can be expected when cadence
  matching waits for the correct presentation cycle. It does not by itself
  indicate an actual end-to-end latency increase. Investigate unexpected
  variation or missed cycles instead.

Keep the guest resolution and refresh realistic for the available memory
bandwidth. Reserve at least two physical CPU cores for Linux and avoid changing
poll intervals as a first response to a spike.
