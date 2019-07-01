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
#include <SDL2/SDL.h>

#include "interface/app.h"
#include "dynamic/renderers.h"
#include "dynamic/clipboards.h"

#include "spice/spice.h"

struct AppState
{
  bool                 running;
  bool                 ignoreInput;
  bool                 escapeActive;
  SDL_Scancode         escapeAction;
  KeybindHandle        bindings[SDL_NUM_SCANCODES];
  bool                 keyDown[SDL_NUM_SCANCODES];

  bool                 haveSrcSize;
  int                  windowW, windowH;
  SDL_Point            srcSize;
  LG_RendererRect      dstRect;
  SDL_Point            cursor;
  bool                 cursorVisible;
  bool                 haveCursorPos;
  float                scaleX, scaleY;
  float                accX, accY;

  const LG_Renderer  * lgr;
  void               * lgrData;
  bool                 lgrResize;

  const LG_Clipboard * lgc;
  SpiceDataType        cbType;
  struct ll          * cbRequestList;

  SDL_Window         * window;
  int                  shmFD;
  struct KVMFRHeader * shm;
  unsigned int         shmSize;

  uint64_t          frameTime;
  uint64_t          lastFrameTime;
  uint64_t          renderTime;
  uint64_t          frameCount;
  uint64_t          renderCount;

  KeybindHandle kbFS;
  KeybindHandle kbInput;
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
  bool         borderless;
  bool         fullscreen;
  bool         maximize;
  bool         minimizeOnFocusLoss;
  bool         center;
  int          x, y;
  unsigned int w, h;
  const char * shmFile;
  unsigned int shmSize;
  unsigned int fpsLimit;
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

  unsigned int cursorPollInterval;
  unsigned int framePollInterval;

  bool         forceRenderer;
  unsigned int forceRendererIndex;

  const char * windowTitle;
  int          mouseSens;
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
