/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include <stdbool.h>
#include <stdatomic.h>
#include <SDL2/SDL.h>
#include <linux/input.h>

#include "interface/app.h"
#include "dynamic/displayservers.h"
#include "dynamic/renderers.h"
#include "common/ivshmem.h"

#include "spice/spice.h"
#include <lgmp/client.h>

enum RunState
{
  APP_STATE_RUNNING,
  APP_STATE_RESTART,
  APP_STATE_SHUTDOWN
};

struct AppState
{
  enum RunState state;

  struct LG_DisplayServerOps * ds;

  bool                 stopVideo;
  bool                 ignoreInput;
  bool                 escapeActive;
  SDL_Scancode         escapeAction;
  KeybindHandle        bindings[KEY_MAX];
  bool                 keyDown[KEY_MAX];

  bool                 haveSrcSize;
  SDL_Point            windowPos;
  int                  windowW, windowH;
  int                  windowCX, windowCY;
  LG_RendererRotate    rotate;
  bool                 focused;
  SDL_Rect             border;
  SDL_Point            srcSize;
  LG_RendererRect      dstRect;
  bool                 posInfoValid;
  bool                 alignToGuest;

  const LG_Renderer  * lgr;
  void               * lgrData;
  atomic_int           lgrResize;

  bool                 cbAvailable;
  SpiceDataType        cbType;
  bool                 cbChunked;
  size_t               cbXfer;
  struct ll          * cbRequestList;

  SDL_SysWMinfo        wminfo;
  SDL_Window         * window;

  struct IVSHMEM       shm;
  PLGMPClient          lgmp;
  PLGMPClientQueue     frameQueue;
  PLGMPClientQueue     pointerQueue;

  bool                  formatValid;
  atomic_uint_least64_t frameTime;
  uint64_t              lastFrameTime;
  uint64_t              renderTime;
  atomic_uint_least64_t frameCount;
  uint64_t              renderCount;


  uint64_t resizeTimeout;
  bool     resizeDone;

  KeybindHandle kbFS;
  KeybindHandle kbVideo;
  KeybindHandle kbRotate;
  KeybindHandle kbInput;
  KeybindHandle kbQuit;
  KeybindHandle kbMouseSensInc;
  KeybindHandle kbMouseSensDec;
  KeybindHandle kbCtrlAltFn[12];
  KeybindHandle kbPass[2];
};

struct AppParams
{
  bool              autoResize;
  bool              allowResize;
  bool              keepAspect;
  bool              forceAspect;
  bool              dontUpscale;
  bool              borderless;
  bool              fullscreen;
  bool              maximize;
  bool              minimizeOnFocusLoss;
  bool              center;
  int               x, y;
  unsigned int      w, h;
  int               fpsMin;
  bool              showFPS;
  LG_RendererRotate winRotate;
  bool              useSpiceInput;
  bool              useSpiceClipboard;
  const char *      spiceHost;
  unsigned int      spicePort;
  bool              clipboardToVM;
  bool              clipboardToLocal;
  bool              scaleMouseInput;
  bool              hideMouse;
  bool              ignoreQuit;
  bool              noScreensaver;
  bool              grabKeyboard;
  bool              grabKeyboardOnFocus;
  SDL_Scancode      escapeKey;
  bool              ignoreWindowsKeys;
  bool              showAlerts;
  bool              captureOnStart;
  bool              quickSplash;
  bool              alwaysShowCursor;

  unsigned int      cursorPollInterval;
  unsigned int      framePollInterval;
  bool              allowDMA;

  bool              forceRenderer;
  unsigned int      forceRendererIndex;

  const char *      windowTitle;
  bool              mouseRedraw;
  int               mouseSens;
  bool              mouseSmoothing;
  bool              rawMouse;
  bool              autoCapture;
  bool              captureInputOnly;
};

struct CBRequest
{
  SpiceDataType       type;
  LG_ClipboardReplyFn replyFn;
  void              * opaque;
};

struct KeybindHandle
{
  SDL_Scancode   key;
  SuperEventFn   callback;
  void         * opaque;
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

  /* the DPI scaling of the guest */
  uint32_t dpiScale;
};

struct DoublePoint
{
  double x, y;
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

  /* true if the guest should be realigned to the host when next drawn */
  bool realign;

  /* true if the cursor needs re-drawing/updating */
  bool redraw;

  /* true if the cursor movements should be scaled */
  bool useScale;

  /* the amount to scale the X & Y movements by */
  struct DoublePoint scale;

  /* the dpi scale factor from the guest as a fraction */
  double dpiScale;

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
};

// forwards
extern struct AppState  g_state;
extern struct AppParams params;
