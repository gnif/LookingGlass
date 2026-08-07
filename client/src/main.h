/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <stdbool.h>
#include <stdatomic.h>
#include <linux/input.h>

#include "dynamic/displayservers.h"
#include "dynamic/renderers.h"
#include "dynamic/audiodev.h"

#include "common/thread.h"
#include "common/types.h"
#include "common/locking.h"
#include "common/ringbuffer.h"
#include "common/event.h"
#include "common/ll.h"

#include <purespice.h>
#include "cimgui.h"
#include "interface/transport.h"

enum RunState
{
  APP_STATE_RUNNING,
  APP_STATE_RESTART,
  APP_STATE_SHUTDOWN
};

enum MicDefaultState {
  MIC_DEFAULT_PROMPT,
  MIC_DEFAULT_ALLOW,
  MIC_DEFAULT_DENY
};
#define MIC_DEFAULT_MAX (MIC_DEFAULT_DENY + 1)

enum AudioResampler {
  AUDIO_RESAMPLER_AUTO,
  AUDIO_RESAMPLER_LIBSAMPLERATE,
  AUDIO_RESAMPLER_BACKEND
};

struct AppState
{
  ImGuiIO        * io;
  ImGuiStyle     * style;
  struct ll      * overlays;
  char           * fontName;
  ImFont         * fontLarge;
  bool             overlayInput;
  ImGuiMouseCursor cursorLast;
  char           * imGuiIni;
  bool             modCtrl;
  bool             modShift;
  bool             modAlt;
  bool             modSuper;
  uint64_t         lastImGuiFrame;
  bool             renderImGuiTwice;
  bool             exclusiveEvdev;

  struct LG_DisplayServerOps * ds;
  bool                         dsInitialized;
  bool                         jitRender;

  uint8_t     spiceUUID[16];
  atomic_bool spiceReady;
  atomic_bool spiceDisplayRequested;
  atomic_bool spiceDisplayActive;
  atomic_bool spiceDisplayTransition;
  bool        spicePrimarySurfaceValid;

  uint8_t guestUUID[16];
  bool    guestUUIDValid;
  LG_TransportGuestOS guestOS;

  atomic_bool lgHostConnected;

  bool                 stopVideo;
  bool                 ignoreInput;
  bool                 escapeActive;
  uint64_t             escapeTime;
  int                  escapeAction;
  bool                 escapeHelp;
  struct ll          * bindings;
  atomic_bool          keyDown[KEY_MAX];

  bool                 haveSrcSize;
  struct Point         windowPos;
  int                  windowW, windowH;
  int                  windowCX, windowCY;
  double               windowScale;
  double               fontScale;
  LG_RendererRotate    rotate;
  bool                 focused;
  struct Border        border;
  struct Point         srcSize;
  LG_RendererRect      dstRect;
  bool                 posInfoValid;
  bool                 alignToGuest;
  atomic_bool          spiceClose;

  atomic_uint_least64_t shaderMousePosition;
  atomic_uint_least32_t shaderMouseState;

  LG_Renderer        * lgr;
  atomic_int           lgrResize;
  atomic_bool          fontDirty;
  LG_Lock              lgrLock;
  bool                 useDMA;

  bool                 cbAvailable;
  _Atomic(PSDataType)  cbRemoteType;
  PSDataType           cbWriteType;
  bool                 cbChunked;
  size_t               cbXfer;
  struct ll          * cbRequestList;

  LG_Transport              * transport;
  const LG_TransportOps     * transportOps;
  LG_TransportFeatureFlags    transportFeatures;

  LGThread            * cursorThread;
  LGThread            * frameThread;
  LGEvent             * frameEvent;
  atomic_bool           invalidateWindow;
  bool                  formatValid;
  uint64_t              frameTime;
  uint64_t              overlayFrameTime;
  uint64_t              lastRenderTime;
  bool                  lastRenderTimeValid;
  RingBuffer            renderTimings;
  RingBuffer            frameLatency;
  uint64_t              frameImportTime;
  uint64_t              frameImportWaitTime;

  atomic_uint_least64_t pendingCount;
  atomic_uint_least64_t renderCount, frameCount;
  _Atomic(float)        fps, ups;

  uint64_t resizeTimeout;
  bool     resizeDone;

  bool     autoIdleInhibitState;

  enum MicDefaultState micDefaultState;

  int spiceHotX, spiceHotY;

  // Set to true when setHDRImageDesc() returns false, indicating
  // the display server could not apply the HDR image description.
  // Checked by the renderer to fall back to software tone-mapping.
  _Atomic(bool) hdrDescFailed;
};

struct AppParams
{
  bool                 autoResize;
  bool                 allowResize;
  bool                 keepAspect;
  bool                 forceAspect;
  bool                 dontUpscale;
  bool                 intUpscale;
  bool                 shrinkOnUpscale;
  bool                 borderless;
  bool                 fullscreen;
  bool                 maximize;
  bool                 minimizeOnFocusLoss;
  bool                 minimizeOnCaptureRelease;
  bool                 center;
  int                  x, y;
  unsigned int         w, h;
  bool                 setGuestRes;
  int                  fpsMin;
  LG_RendererRotate    winRotate;
  bool                 useSpice;
  bool                 useSpiceInput;
  bool                 useSpiceClipboard;
  bool                 useSpiceAudio;
  const char *         spiceHost;
  unsigned int         spicePort;
  bool                 clipboardToVM;
  bool                 clipboardToLocal;
  bool                 scaleMouseInput;
  bool                 hideMouse;
  bool                 ignoreQuit;
  bool                 noScreensaver;
  bool                 autoScreensaver;
  bool                 captureOnFocus;
  bool                 grabKeyboard;
  bool                 grabKeyboardOnFocus;
  int                  escapeKey;
  bool                 ignoreWindowsKeys;
  bool                 releaseKeysOnFocusLoss;
  bool                 showAlerts;
  bool                 captureOnStart;
  bool                 quickSplash;
  bool                 overlayDim;
  bool                 alwaysShowCursor;
  uint64_t             helpMenuDelayUs;
  const char *         uiFont;
  int                  uiSize;
  bool                 jitRender;
  bool                 requestActivation;
  bool                 disableWaitingMessage;

  const char         * transport;

  bool                 forceRenderer;
  unsigned int         forceRendererIndex;

  const char *         windowTitle;
  const char *         appId;
  bool                 mouseRedraw;
  bool                 mouseTrace;
  int                  mouseSens;
  bool                 mouseSmoothing;
  bool                 rawMouse;
  bool                 autoCapture;
  bool                 captureInputOnly;
  bool                 showCursorDot;
  bool                 largeCursorDot;

  bool                 audioDebug;
  int                  audioPeriodSize;
  int                  audioLatencyOffset;
  enum AudioResampler  audioResampler;
  bool                 micShowIndicator;
  enum MicDefaultState micDefaultState;
  bool                 audioSyncVolume;
};

struct CBRequest
{
  PSDataType          type;
  LG_ClipboardReplyFn replyFn;
  void              * opaque;
};

struct KeybindHandle
{
  int          sc;
  KeybindFn    callback;
  const char * description;
  void *       opaque;
};

enum WarpState
{
  WARP_STATE_ON,
  WARP_STATE_OFF
};

struct CursorInfo
{
  /* x & y postiion */
  int  x , y;

  /* pointer hotspot offsets */
  int  hx, hy;

  /* true if the pointer is visible on the guest */
  bool visible;

  /* true if the details in this struct are valid */
  bool valid;
};

struct CursorState
{
  /* cursor is in grab mode */
  bool grab;

  /* true if we are to draw the cursor on screen */
  bool draw;

  /* true if the cursor is currently in our window */
  bool inWindow;

  /* true if the cursor is currently in the guest view area */
  bool inView;

  /* true if the cursor should be confined to the guest view area */
  bool viewReq;

  /* true if a pointer exit is waiting for confinement to release */
  bool exit;

  /* true if a Wayland surface exit is waiting for a real leave */
  bool surfaceExit;

  /* the local pointer exit target */
  struct DoublePoint exitPos;

  /* true if the guest should be realigned to the host when next drawn */
  bool realign;

  /* true if the guest is currently realigning to the host */
  bool realigning;

  /* true if the cursor needs re-drawing/updating */
  bool redraw;

  /* true if the cursor movements should be scaled */
  bool useScale;

  /* the amount to scale the X & Y movements by */
  struct DoublePoint scale;

  /* the error accumulator */
  struct DoublePoint acc;

  /* the local position */
  struct DoublePoint pos;

  /* true if the position is valid */
  bool valid;

  /* the button state */
  unsigned int buttons;

  /* the delta since last warp when in auto capture mode */
  struct DoublePoint delta;

  /* the scale factor for the mouse sensitiviy */
  int sens;

  /* the mouse warp state */
  enum WarpState warpState;

  /* the guest's cursor position */
  struct CursorInfo guest;

  /* the projected position after move, for app_handleMouseBasic only */
  struct Point projected;
};

// forwards
extern struct AppState    g_state;
extern struct CursorState g_cursor;
extern struct AppParams   g_params;

static inline enum RunState app_getState(void)
{
  extern _Atomic(enum RunState) p_appState;
  return atomic_load_explicit(&p_appState, memory_order_acquire);
}

static inline void app_setState(enum RunState state)
{
  extern _Atomic(enum RunState) p_appState;
  atomic_store_explicit(&p_appState, state, memory_order_release);
}

int main_cursorThread(void * unused);
int main_frameThread(void * unused);

#define RENDERER(fn, ...) g_state.lgr->ops.fn(g_state.lgr, ##__VA_ARGS__)
