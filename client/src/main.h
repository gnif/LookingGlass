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

#include "cimgui.h"
#include "app.h"
#include "interface/transport.h"
#include "transport_fallback.h"

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

struct VideoSourceState
{
  atomic_uint_least64_t generation;
  atomic_uint_least64_t transitionSerial;
  atomic_bool           transitionPending;
  atomic_uint_least32_t width;
  atomic_uint_least32_t height;
  atomic_int            rotate;
  atomic_uint_least32_t appliedWidth;
  atomic_uint_least32_t appliedHeight;
  atomic_int            appliedRotate;
  atomic_bool           ready;
  bool                  swSurface;
  bool                  configurePending;

  bool     cursorStateValid;
  bool     cursorVisible;
  int      cursorX;
  int      cursorY;
  int      cursorHX;
  int      cursorHY;
  uint32_t cursorWhiteLevel;
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

  struct VideoSourceState videoSource[LG_VIDEO_SOURCE_COUNT];
  atomic_int               videoSourceRequested;
  atomic_int               videoSourceApplied;
  atomic_uint_least64_t     videoSourceAppliedGeneration;
  LG_Lock                   videoSourceLock;
  LG_Lock                   videoSplashLock;
  bool                      videoGeometryDirty;

  atomic_bool fallbackUUIDMismatch;
  atomic_uint transportLost;

  uint8_t guestUUID[16];
  bool    guestUUIDValid;
  LG_TransportGuestOS guestOS;

  atomic_bool lgHostConnected;

  bool                 stopVideo;
  bool                 ignoreInput;
  bool                 escapeActive;
  uint64_t             escapeTime;
  int                  escapeAction;
  bool                 escapeKeys[KEY_MAX];
  bool                 escapeHelp;
  struct ll          * bindings;
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
  atomic_uint_least64_t shaderMousePosition;
  atomic_uint_least32_t shaderMouseState;

  LG_Renderer        * lgr;
  atomic_int           lgrResize;
  atomic_bool          fontDirty;
  LG_Lock              lgrLock;
  bool                 useDMA;

  LG_TransportInstance      transport;
  const LG_VideoOps       * videoOps;
  LG_TransportFeatureFlags  transportFeatures;

  LG_TransportFallback * fallback;

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
  bool                 center;
  int                  x, y;
  unsigned int         w, h;
  bool                 setGuestRes;
  int                  fpsMin;
  LG_RendererRotate    winRotate;
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
  atomic_bool redraw;

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

  /* true if the last local position can be used to predict motion */
  bool motionValid;

  /* true if auto capture currently requests the keyboard */
  bool autoCaptureActive;

  /* the button state */
  unsigned int buttons;

  /* the scale factor for the mouse sensitiviy */
  int sens;

  /* the mouse warp state */
  enum WarpState warpState;

  /* the guest's cursor position */
  struct CursorInfo guest;
};

// forwards
extern struct AppState    g_state;
extern struct CursorState g_cursor;
extern struct AppParams   g_params;

int main_cursorThread(void * unused);
int main_frameThread(void * unused);

#define RENDERER(fn, ...) g_state.lgr->ops.fn(g_state.lgr, ##__VA_ARGS__)
