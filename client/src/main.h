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

#include "interface/app.h"
#include "dynamic/renderers.h"
#include "dynamic/clipboards.h"
#include "common/ivshmem.h"

#include "spice/spice.h"
#include <lgmp/client.h>

enum RunState
{
  APP_STATE_RUNNING,
  APP_STATE_RESTART,
  APP_STATE_SHUTDOWN
};

struct CursorInfo
{
  int x , y;
  int hx, hy;
};

enum WarpState
{
  WARP_STATE_ARMED,
  WARP_STATE_ON,
  WARP_STATE_ACTIVE,
  WARP_STATE_OFF
};

struct AppState
{
  enum RunState        state;
  bool                 ignoreInput;
  bool                 escapeActive;
  SDL_Scancode         escapeAction;
  KeybindHandle        bindings[SDL_NUM_SCANCODES];
  bool                 keyDown[SDL_NUM_SCANCODES];

  bool                 haveSrcSize;
  int                  windowW, windowH;
  SDL_Point            srcSize;
  LG_RendererRect      dstRect;
  struct CursorInfo    cursor;
  bool                 cursorVisible;

  bool  serverMode;
  bool  haveCursorPos;
  bool  drawCursor;
  bool  cursorInView;
  bool  updateCursor;
  bool  initialCursorSync;
  float scaleX, scaleY;
  float accX, accY;
  int   curLastX;
  int   curLastY;
  bool  haveCurLocal;
  int   curLocalX;
  int   curLocalY;
  bool  haveAligned;

  enum WarpState   warpState;
  int   warpFromX, warpFromY;
  int   warpToX  , warpToY;

  const LG_Renderer  * lgr;
  void               * lgrData;
  bool                 lgrResize;

  const LG_Clipboard * lgc;
  SpiceDataType        cbType;
  struct ll          * cbRequestList;

  SDL_SysWMinfo        wminfo;
  SDL_Window         * window;

  struct IVSHMEM       shm;
  PLGMPClient          lgmp;
  PLGMPClientQueue     frameQueue;
  PLGMPClientQueue     pointerQueue;

  atomic_uint_least64_t frameTime;
  uint64_t              lastFrameTime;
  uint64_t              renderTime;
  uint64_t              frameCount;
  uint64_t              renderCount;


  uint64_t resizeTimeout;
  bool     resizeDone;

  KeybindHandle kbFS;
  KeybindHandle kbInput;
  KeybindHandle kbQuit;
  KeybindHandle kbMouseSensInc;
  KeybindHandle kbMouseSensDec;
  KeybindHandle kbCtrlAltFn[12];

  int   mouseSens;
  float sensX, sensY;
};

struct AppParams
{
  bool         autoResize;
  bool         allowResize;
  bool         keepAspect;
  bool         forceAspect;
  bool         borderless;
  bool         fullscreen;
  bool         maximize;
  bool         minimizeOnFocusLoss;
  bool         center;
  int          x, y;
  unsigned int w, h;
  unsigned int fpsMin;
  bool         showFPS;
  bool         useSpiceInput;
  bool         useSpiceClipboard;
  const char * spiceHost;
  unsigned int spicePort;
  bool         clipboardToVM;
  bool         clipboardToLocal;
  bool         scaleMouseInput;
  bool         hideMouse;
  bool         ignoreQuit;
  bool         noScreensaver;
  bool         grabKeyboard;
  SDL_Scancode escapeKey;
  bool         showAlerts;
  bool         captureOnStart;

  unsigned int cursorPollInterval;
  unsigned int framePollInterval;

  bool         forceRenderer;
  unsigned int forceRendererIndex;

  const char * windowTitle;
  int          mouseSens;
  bool         mouseRedraw;
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

// forwards
extern struct AppState  state;
extern struct AppParams params;
