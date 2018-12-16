/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
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

#include <getopt.h>
#include <signal.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <stdio.h>
#include <stdlib.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <libconfig.h>

#include "debug.h"
#include "utils.h"
#include "KVMFR.h"
#include "spice/spice.h"
#include "kb.h"

#include "lg-renderers.h"

struct AppState
{
  bool                 running;
  bool                 started;
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

  const LG_Renderer  * lgr ;
  void               * lgrData;
  bool                 lgrResize;

  SDL_Window         * window;
  int                  shmFD;
  struct KVMFRHeader * shm;
  unsigned int         shmSize;

  uint64_t          frameTime;
  uint64_t          lastFrameTime;
  uint64_t          renderTime;
  uint64_t          frameCount;
  uint64_t          renderCount;
};

typedef struct RenderOpts
{
  unsigned int          size;
  unsigned int          argc;
  LG_RendererOptValue * argv;
}
RendererOpts;

struct AppParams
{
  const char * configFile;
  bool         autoResize;
  bool         allowResize;
  bool         keepAspect;
  bool         borderless;
  bool         fullscreen;
  bool         center;
  int          x, y;
  unsigned int w, h;
  char       * shmFile;
  unsigned int shmSize;
  unsigned int fpsLimit;
  bool         showFPS;
  bool         useSpice;
  char       * spiceHost;
  unsigned int spicePort;
  bool         scaleMouseInput;
  bool         hideMouse;
  bool         ignoreQuit;
  bool         allowScreensaver;
  bool         grabKeyboard;
  SDL_Scancode captureKey;
  bool         disableAlerts;

  bool         forceRenderer;
  unsigned int forceRendererIndex;
  RendererOpts rendererOpts[LG_RENDERER_COUNT];
};

struct AppState  state;
struct AppParams params =
{
  .configFile       = "/etc/looking-glass.conf",
  .autoResize       = false,
  .allowResize      = true,
  .keepAspect       = true,
  .borderless       = false,
  .fullscreen       = false,
  .center           = true,
  .x                = 0,
  .y                = 0,
  .w                = 1024,
  .h                = 768,
  .shmFile          = "/dev/shm/looking-glass",
  .shmSize          = 0,
  .fpsLimit         = 200,
  .showFPS          = false,
  .useSpice         = true,
  .spiceHost        = "127.0.0.1",
  .spicePort        = 5900,
  .scaleMouseInput  = true,
  .hideMouse        = true,
  .ignoreQuit       = false,
  .allowScreensaver = true,
  .grabKeyboard     = true,
  .captureKey       = SDL_SCANCODE_SCROLLLOCK,
  .disableAlerts    = false,
  .forceRenderer    = false
};

static void updatePositionInfo()
{
  if (state.haveSrcSize)
  {
    if (params.keepAspect)
    {
      const float srcAspect = (float)state.srcSize.y / (float)state.srcSize.x;
      const float wndAspect = (float)state.windowH / (float)state.windowW;
      if (wndAspect < srcAspect)
      {
        state.dstRect.w = (float)state.windowH / srcAspect;
        state.dstRect.h = state.windowH;
        state.dstRect.x = (state.windowW >> 1) - (state.dstRect.w >> 1);
        state.dstRect.y = 0;
      }
      else
      {
        state.dstRect.w = state.windowW;
        state.dstRect.h = (float)state.windowW * srcAspect;
        state.dstRect.x = 0;
        state.dstRect.y = (state.windowH >> 1) - (state.dstRect.h >> 1);
      }
    }
    else
    {
      state.dstRect.x = 0;
      state.dstRect.y = 0;
      state.dstRect.w = state.windowW;
      state.dstRect.h = state.windowH;
    }
    state.dstRect.valid = true;

    state.scaleX = (float)state.srcSize.y / (float)state.dstRect.h;
    state.scaleY = (float)state.srcSize.x / (float)state.dstRect.w;
  }

  state.lgrResize = true;
}

int renderThread(void * unused)
{
  if (!state.lgr->render_startup(state.lgrData, state.window))
    return 1;

  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);

  while(state.running)
  {
    if (state.lgrResize)
    {
      if (state.lgr)
        state.lgr->on_resize(state.lgrData, state.windowW, state.windowH, state.dstRect);
      state.lgrResize = false;
    }

    if (!state.lgr->render(state.lgrData, state.window))
      break;

    if (params.showFPS)
    {
      const uint64_t t    = nanotime();
      state.renderTime   += t - state.lastFrameTime;
      state.lastFrameTime = t;
      ++state.renderCount;

      if (state.renderTime > 1e9)
      {
        const float avgUPS = 1000.0f / (((float)state.renderTime / state.frameCount ) / 1e6f);
        const float avgFPS = 1000.0f / (((float)state.renderTime / state.renderCount) / 1e6f);
        state.lgr->update_fps(state.lgrData, avgUPS, avgFPS);

        state.renderTime  = 0;
        state.frameCount  = 0;
        state.renderCount = 0;
      }
    }

    uint64_t nsec = time.tv_nsec + state.frameTime;
    if (nsec > 1e9)
    {
      time.tv_nsec = nsec - 1e9;
      ++time.tv_sec;
    }
    else
      time.tv_nsec = nsec;

    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &time, NULL);
  }

  return 0;
}

int cursorThread(void * unused)
{
  KVMFRCursor         header;
  LG_RendererCursor   cursorType     = LG_CURSOR_COLOR;
  uint32_t            version        = 0;

  memset(&header, 0, sizeof(KVMFRCursor));

  while(state.running)
  {
    // poll until we have cursor data
    if(!(state.shm->cursor.flags & KVMFR_CURSOR_FLAG_UPDATE))
    {
      if (!state.running)
        return 0;

      usleep(1);
      continue;
    }

    // we must take a copy of the header to prevent the contained arguments
    // from being abused to overflow buffers.
    memcpy(&header, &state.shm->cursor, sizeof(struct KVMFRCursor));

    if (header.flags & KVMFR_CURSOR_FLAG_SHAPE &&
        header.version != version)
    {
      version = header.version;

      bool bad = false;
      switch(header.type)
      {
        case CURSOR_TYPE_COLOR       : cursorType = LG_CURSOR_COLOR       ; break;
        case CURSOR_TYPE_MONOCHROME  : cursorType = LG_CURSOR_MONOCHROME  ; break;
        case CURSOR_TYPE_MASKED_COLOR: cursorType = LG_CURSOR_MASKED_COLOR; break;
        default:
          DEBUG_ERROR("Invalid cursor type");
          bad = true;
          break;
      }

      if (bad)
        break;

      // check the data position is sane
      const uint64_t dataSize = header.height * header.pitch;
      if (header.dataPos + dataSize > state.shmSize)
      {
        DEBUG_ERROR("The guest sent an invalid mouse dataPos");
        break;
      }

      const uint8_t * data = (const uint8_t *)state.shm + header.dataPos;
      if (!state.lgr->on_mouse_shape(
        state.lgrData,
        cursorType,
        header.width,
        header.height,
        header.pitch,
        data)
      )
      {
        DEBUG_ERROR("Failed to update mouse shape");
        break;
      }
    }

    // now we have taken the mouse data, we can flag to the host we are ready
    state.shm->cursor.flags = 0;

    bool showCursor = header.flags & KVMFR_CURSOR_FLAG_VISIBLE;
    if (header.flags & KVMFR_CURSOR_FLAG_POS)
    {
      state.cursor.x      = header.x;
      state.cursor.y      = header.y;
      state.haveCursorPos = true;
    }

    if (showCursor != state.cursorVisible || header.flags & KVMFR_CURSOR_FLAG_POS)
    {
      state.cursorVisible = showCursor;
      state.lgr->on_mouse_event
      (
        state.lgrData,
        state.cursorVisible,
        state.cursor.x,
        state.cursor.y
      );
    }
  }

  return 0;
}

int frameThread(void * unused)
{
  bool       error = false;
  KVMFRFrame header;

  memset(&header, 0, sizeof(struct KVMFRFrame));
  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);

  while(state.running)
  {
    // poll until we have a new frame
    while(!(state.shm->frame.flags & KVMFR_FRAME_FLAG_UPDATE))
    {
      if (!state.running)
        break;

      usleep(1);
      continue;
    }

    // we must take a copy of the header to prevent the contained
    // arguments from being abused to overflow buffers.
    memcpy(&header, &state.shm->frame, sizeof(struct KVMFRFrame));

    // tell the host to continue as the host buffers up to one frame
    // we can be sure the data for this frame wont be touched
    __sync_and_and_fetch(&state.shm->frame.flags, ~KVMFR_FRAME_FLAG_UPDATE);

    // sainty check of the frame format
    if (
      header.type    >= FRAME_TYPE_MAX ||
      header.width   == 0 ||
      header.height  == 0 ||
      header.pitch   == 0 ||
      header.dataPos == 0 ||
      header.dataPos > state.shmSize ||
      header.pitch   < header.width
    ){
      DEBUG_WARN("Bad header");
      usleep(1000);
      continue;
    }

    // setup the renderer format with the frame format details
    LG_RendererFormat lgrFormat;
    lgrFormat.type   = header.type;
    lgrFormat.width  = header.width;
    lgrFormat.height = header.height;
    lgrFormat.stride = header.stride;
    lgrFormat.pitch  = header.pitch;

    size_t dataSize;
    switch(header.type)
    {
      case FRAME_TYPE_RGBA:
      case FRAME_TYPE_BGRA:
      case FRAME_TYPE_RGBA10:
        dataSize       = lgrFormat.height * lgrFormat.pitch;
        lgrFormat.bpp  = 32;
        break;

      case FRAME_TYPE_YUV420:
        dataSize       = lgrFormat.height * lgrFormat.width;
        dataSize      += (dataSize / 4) * 2;
        lgrFormat.bpp  = 12;
        break;

      default:
        DEBUG_ERROR("Unsupported frameType");
        error = true;
        break;
    }

    if (error)
      break;

    // check the header's dataPos is sane
    if (header.dataPos + dataSize > state.shmSize)
    {
      DEBUG_ERROR("The guest sent an invalid dataPos");
      break;
    }

    if (header.width != state.srcSize.x || header.height != state.srcSize.y)
    {
      state.srcSize.x = header.width;
      state.srcSize.y = header.height;
      state.haveSrcSize = true;
      if (params.autoResize)
        SDL_SetWindowSize(state.window, header.width, header.height);
      updatePositionInfo();
    }

    const uint8_t * data = (const uint8_t *)state.shm + header.dataPos;
    if (!state.lgr->on_frame_event(state.lgrData, lgrFormat, data))
    {
      DEBUG_ERROR("renderer on frame event returned failure");
      break;
    }

    ++state.frameCount;
    if (!state.started)
    {
      state.started = true;
      updatePositionInfo();
    }
  }

  state.running = false;
  return 0;
}

int spiceThread(void * arg)
{
  while(state.running)
    if (!spice_process())
    {
      if (state.running)
      {
        state.running = false;
        DEBUG_ERROR("failed to process spice messages");
      }
      break;
    }

  state.running = false;
  return 0;
}

static inline const uint32_t mapScancode(SDL_Scancode scancode)
{
  uint32_t ps2;
  if (scancode > (sizeof(usb_to_ps2) / sizeof(uint32_t)) || (ps2 = usb_to_ps2[scancode]) == 0)
  {
    DEBUG_WARN("Unable to map USB scan code: %x\n", scancode);
    return 0;
  }
  return ps2;
}

int eventFilter(void * userdata, SDL_Event * event)
{
  static bool serverMode   = false;
  static bool realignGuest = true;

  switch(event->type)
  {
    case SDL_QUIT:
    {
      if (!params.ignoreQuit)
        state.running = false;
      return 0;
    }

    case SDL_WINDOWEVENT:
    {
      switch(event->window.event)
      {
        case SDL_WINDOWEVENT_ENTER:
          realignGuest = true;
          break;

        case SDL_WINDOWEVENT_SIZE_CHANGED:
          SDL_GetWindowSize(state.window, &state.windowW, &state.windowH);
          updatePositionInfo();
          realignGuest = true;
          break;
      }
      return 0;
    }
  }

  if (!params.useSpice)
    return 0;

  switch(event->type)
  {
    case SDL_MOUSEMOTION:
    {
      if (
        !serverMode && (
          event->motion.x < state.dstRect.x                   ||
          event->motion.x > state.dstRect.x + state.dstRect.w ||
          event->motion.y < state.dstRect.y                   ||
          event->motion.y > state.dstRect.y + state.dstRect.h
        )
      )
      {
        realignGuest = true;
        break;
      }

      int x = 0;
      int y = 0;
      if (realignGuest && state.haveCursorPos)
      {
        x = event->motion.x - state.dstRect.x;
        y = event->motion.y - state.dstRect.y;
        if (params.scaleMouseInput && !serverMode)
        {
          x = (float)x * state.scaleX;
          y = (float)y * state.scaleY;
        }
        x -= state.cursor.x;
        y -= state.cursor.y;
        realignGuest = false;
        state.accX = 0;
        state.accY = 0;

        if (!spice_mouse_motion(x, y))
          DEBUG_ERROR("SDL_MOUSEMOTION: failed to send message");
        break;
      }

      x = event->motion.xrel;
      y = event->motion.yrel;
      if (x != 0 || y != 0)
      {
        if (params.scaleMouseInput && !serverMode)
        {
          state.accX += (float)x * state.scaleX;
          state.accY += (float)y * state.scaleY;
          x = floor(state.accX);
          y = floor(state.accY);
          state.accX -= x;
          state.accY -= y;
        }

        if (!spice_mouse_motion(x, y))
        {
          DEBUG_ERROR("SDL_MOUSEMOTION: failed to send message");
          break;
        }
      }

      break;
    }

    case SDL_KEYDOWN:
    {
      SDL_Scancode sc = event->key.keysym.scancode;
      if (sc == params.captureKey)
      {
        if (event->key.repeat)
          break;

        serverMode = !serverMode;
        spice_mouse_mode(serverMode);
        SDL_SetRelativeMouseMode(serverMode);
        SDL_SetWindowGrab(state.window, serverMode);
        DEBUG_INFO("Server Mode: %s", serverMode ? "on" : "off");

        if (state.lgr && !params.disableAlerts)
          state.lgr->on_alert(
            state.lgrData,
            serverMode ? LG_ALERT_SUCCESS  : LG_ALERT_WARNING,
            serverMode ? "Capture Enabled" : "Capture Disabled",
            NULL
          );

        if (!serverMode)
          realignGuest = true;
        break;
      }

      uint32_t scancode = mapScancode(sc);
      if (scancode == 0)
        break;

      if (!state.keyDown[sc])
      {
        if (spice_key_down(scancode))
          state.keyDown[sc] = true;
        else
        {
          DEBUG_ERROR("SDL_KEYDOWN: failed to send message");
          break;
        }
      }
      break;
    }

    case SDL_KEYUP:
    {
      SDL_Scancode sc = event->key.keysym.scancode;
      if (sc == params.captureKey)
        break;

      // avoid sending key up events when we didn't send a down
      if (!state.keyDown[sc])
        break;

      uint32_t scancode = mapScancode(sc);
      if (scancode == 0)
        break;

      if (spice_key_up(scancode))
        state.keyDown[sc] = false;
      else
      {
        DEBUG_ERROR("SDL_KEYUP: failed to send message");
        break;
      }
      break;
    }

    case SDL_MOUSEWHEEL:
      if (
        !spice_mouse_press  (event->wheel.y == 1 ? 4 : 5) ||
        !spice_mouse_release(event->wheel.y == 1 ? 4 : 5)
        )
      {
        DEBUG_ERROR("SDL_MOUSEWHEEL: failed to send messages");
        break;
      }
      break;

    case SDL_MOUSEBUTTONDOWN:
      // The SPICE protocol doesn't support more than a standard PS/2 3 button mouse
      if (event->button.button > 3)
        break;
      if (
        !spice_mouse_position(event->button.x, event->button.y) ||
        !spice_mouse_press(event->button.button)
      )
      {
        DEBUG_ERROR("SDL_MOUSEBUTTONDOWN: failed to send message");
        break;
      }
      break;

    case SDL_MOUSEBUTTONUP:
      // The SPICE protocol doesn't support more than a standard PS/2 3 button mouse
      if (event->button.button > 3)
        break;
      if (
        !spice_mouse_position(event->button.x, event->button.y) ||
        !spice_mouse_release(event->button.button)
      )
      {
        DEBUG_ERROR("SDL_MOUSEBUTTONUP: failed to send message");
        break;
      }
      break;
  }

  // consume all events
  return 0;
}

void intHandler(int signal)
{
  switch(signal)
  {
    case SIGINT:
      state.running = false;
      break;
  }
}

static void * map_memory()
{
  struct stat st;
  if (stat(params.shmFile, &st) < 0)
  {
    DEBUG_ERROR("Failed to stat the shared memory file: %s", params.shmFile);
    return NULL;
  }

  state.shmSize = params.shmSize ? params.shmSize : st.st_size;
  state.shmFD   = open(params.shmFile, O_RDWR, (mode_t)0600);
  if (state.shmFD < 0)
  {
    DEBUG_ERROR("Failed to open the shared memory file: %s", params.shmFile);
    return NULL;
  }

  void * map = mmap(0, state.shmSize, PROT_READ | PROT_WRITE, MAP_SHARED, state.shmFD, 0);
  if (map == MAP_FAILED)
  {
    DEBUG_ERROR("Failed to map the shared memory file: %s", params.shmFile);
    close(state.shmFD);
    state.shmFD = 0;
    return NULL;
  }

  return map;
}

static bool try_renderer(const int index, const LG_RendererParams lgrParams, Uint32 * sdlFlags)
{
  const LG_Renderer *r    = LG_Renderers[index];
  RendererOpts      *opts = &params.rendererOpts[index];

  if (!IS_LG_RENDERER_VALID(r))
  {
    DEBUG_ERROR("FIXME: Renderer %d is invalid, skipping", index);
    return false;
  }

  // create the renderer
  state.lgrData = NULL;
  if (!r->create(&state.lgrData, lgrParams))
    return false;

  // set it's options
  for(unsigned int i = 0; i < opts->argc; ++i)
    opts->argv[i].opt->handler(state.lgrData, opts->argv[i].value);

  // initialize the renderer
  if (!r->initialize(state.lgrData, sdlFlags))
  {
    r->deinitialize(state.lgrData);
    return false;
  }

  DEBUG_INFO("Using Renderer: %s", r->get_name());
  return true;
}

int run()
{
  DEBUG_INFO("Looking Glass (" BUILD_VERSION ")");
  DEBUG_INFO("Locking Method: " LG_LOCK_MODE);

  memset(&state, 0, sizeof(state));
  state.running   = true;
  state.scaleX    = 1.0f;
  state.scaleY    = 1.0f;
  state.frameTime = 1e9 / params.fpsLimit;

  char* XDG_SESSION_TYPE = getenv("XDG_SESSION_TYPE");

  if (XDG_SESSION_TYPE == NULL) {
    XDG_SESSION_TYPE = "unspecified";
  }

  if (strcmp(XDG_SESSION_TYPE, "wayland") == 0) {
     DEBUG_INFO("Wayland detected");
     int err = setenv("SDL_VIDEODRIVER", "wayland", 1);
     if (err < 0) {
       DEBUG_ERROR("Unable to set the env variable SDL_VIDEODRIVER: %d", err);
       return -1;
     }
     DEBUG_INFO("SDL_VIDEODRIVER has been set to wayland");
  }

  // warn about using FPS display until we can fix the font rendering to prevent lag spikes
  if (params.showFPS)
  {
    DEBUG_WARN("================================================================================");
    DEBUG_WARN("WARNING: The FPS display causes microstutters, this is a known issue"            );
    DEBUG_WARN("================================================================================");
  }

  if (SDL_Init(SDL_INIT_VIDEO) < 0)
  {
    DEBUG_ERROR("SDL_Init Failed");
    return -1;
  }

  // override SDL's SIGINIT handler so that we can tell the difference between
  // SIGINT and the user sending a close event, such as ALT+F4
  signal(SIGINT, intHandler);

  LG_RendererParams lgrParams;
  lgrParams.showFPS = params.showFPS;
  Uint32 sdlFlags;

  if (params.forceRenderer)
  {
    DEBUG_INFO("Trying forced renderer");
    sdlFlags = 0;
    if (!try_renderer(params.forceRendererIndex, lgrParams, &sdlFlags))
    {
      DEBUG_ERROR("Forced renderer failed to iniailize");
      return -1;
    }
    state.lgr = LG_Renderers[params.forceRendererIndex];
  }
  else
  {
    // probe for a a suitable renderer
    for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
    {
      sdlFlags = 0;
      if (try_renderer(i, lgrParams, &sdlFlags))
      {
        state.lgr = LG_Renderers[i];
        DEBUG_INFO("Using: %s", state.lgr->get_name());
        break;
      }
    }
  }

  if (!state.lgr)
  {
    DEBUG_INFO("Unable to find a suitable renderer");
    return -1;
  }

  state.window = SDL_CreateWindow(
    "Looking Glass (Client)",
    params.center ? SDL_WINDOWPOS_CENTERED : params.x,
    params.center ? SDL_WINDOWPOS_CENTERED : params.y,
    params.w,
    params.h,
    (
      SDL_WINDOW_SHOWN |
      (params.fullscreen  ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) |
      (params.allowResize ? SDL_WINDOW_RESIZABLE  : 0) |
      (params.borderless  ? SDL_WINDOW_BORDERLESS : 0) |
      sdlFlags
    )
  );

  if (state.window == NULL) {
    DEBUG_ERROR("Could not create an SDL window: %s\n", SDL_GetError());
    return 1;
  }

  if (params.fullscreen)
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

  if (params.allowScreensaver)
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");

  if (!params.center)
    SDL_SetWindowPosition(state.window, params.x, params.y);

  // set the compositor hint to bypass for low latency
  SDL_SysWMinfo wminfo;
  SDL_VERSION(&wminfo.version);
  if (SDL_GetWindowWMInfo(state.window, &wminfo))
  {
    if (wminfo.subsystem == SDL_SYSWM_X11)
    {
      Atom NETWM_BYPASS_COMPOSITOR = XInternAtom(
        wminfo.info.x11.display,
        "NETWM_BYPASS_COMPOSITOR",
        False);

      unsigned long value = 1;
      XChangeProperty(
        wminfo.info.x11.display,
        wminfo.info.x11.window,
        NETWM_BYPASS_COMPOSITOR,
        XA_CARDINAL,
        32,
        PropModeReplace,
        (unsigned char *)&value,
        1
      );
    }
  } else {
    DEBUG_ERROR("Could not get SDL window information %s", SDL_GetError());
    return -1;
  }

  if (!state.window)
  {
    DEBUG_ERROR("failed to create window");
    return -1;
  }

  SDL_Cursor *cursor = NULL;
  if (params.hideMouse)
  {
    // work around SDL_ShowCursor being non functional
    int32_t cursorData[2] = {0, 0};
    cursor = SDL_CreateCursor((uint8_t*)cursorData, (uint8_t*)cursorData, 8, 8, 4, 4);
    SDL_SetCursor(cursor);
    SDL_ShowCursor(SDL_DISABLE);
  }

  SDL_Thread *t_spice  = NULL;
  SDL_Thread *t_main   = NULL;
  SDL_Thread *t_frame  = NULL;
  SDL_Thread *t_render = NULL;

  while(1)
  {
    state.shm = (struct KVMFRHeader *)map_memory();
    if (!state.shm)
    {
      DEBUG_ERROR("Failed to map memory");
      break;
    }

    // start the renderThread so we don't just display junk
    if (!(t_render = SDL_CreateThread(renderThread, "renderThread", NULL)))
    {
      DEBUG_ERROR("render create thread failed");
      break;
    }

    if (params.useSpice)
    {
      if (!spice_connect(params.spiceHost, params.spicePort, ""))
      {
        DEBUG_ERROR("Failed to connect to spice server");
        return 0;
      }

      while(state.running && !spice_ready())
        if (!spice_process())
        {
          state.running = false;
          DEBUG_ERROR("Failed to process spice messages");
          break;
        }

      if (!(t_spice = SDL_CreateThread(spiceThread, "spiceThread", NULL)))
      {
        DEBUG_ERROR("spice create thread failed");
        break;
      }
    }

    // ensure mouse acceleration is identical in server mode
    SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "1", SDL_HINT_OVERRIDE);
    SDL_SetEventFilter(eventFilter, NULL);

    // flag the host that we are starting up this is important so that
    // the host wakes up if it is waiting on an interrupt, the host will
    // also send us the current mouse shape since we won't know it yet
    DEBUG_INFO("Waiting for host to signal it's ready...");
    __sync_or_and_fetch(&state.shm->flags, KVMFR_HEADER_FLAG_RESTART);

    while(state.running && (state.shm->flags & KVMFR_HEADER_FLAG_RESTART))
      SDL_WaitEventTimeout(NULL, 1000);

    if (!state.running)
      break;

    DEBUG_INFO("Host ready, starting session");

    // check the header's magic and version are valid
    if (memcmp(state.shm->magic, KVMFR_HEADER_MAGIC, sizeof(KVMFR_HEADER_MAGIC)) != 0)
    {
      DEBUG_ERROR("Invalid header magic, is the host running?");
      break;
    }

    if (state.shm->version != KVMFR_HEADER_VERSION)
    {
      DEBUG_ERROR("KVMFR version missmatch, expected %u but got %u", KVMFR_HEADER_VERSION, state.shm->version);
      DEBUG_ERROR("This is not a bug, ensure you have the right version of looking-glass-host.exe on the guest");
      break;
    }


    if (!(t_main = SDL_CreateThread(cursorThread, "cursorThread", NULL)))
    {
      DEBUG_ERROR("cursor create thread failed");
      break;
    }

    if (!(t_frame = SDL_CreateThread(frameThread, "frameThread", NULL)))
    {
      DEBUG_ERROR("frame create thread failed");
      break;
    }

    bool *closeAlert = NULL;
    while(state.running)
    {
      SDL_WaitEventTimeout(NULL, 1000);

      if (closeAlert == NULL)
      {
        if (state.shm->flags & KVMFR_HEADER_FLAG_PAUSED)
        {
          if (state.lgr && !params.disableAlerts)
            state.lgr->on_alert(
              state.lgrData,
              LG_ALERT_WARNING,
              "Stream Paused",
              &closeAlert
            );
        }
      }
      else
      {
        if (!(state.shm->flags & KVMFR_HEADER_FLAG_PAUSED))
        {
          *closeAlert = true;
          closeAlert  = NULL;
        }
      }
    }

    break;
  }

  state.running = false;

  if (t_render)
    SDL_WaitThread(t_render, NULL);

  if (t_frame)
    SDL_WaitThread(t_frame, NULL);

  if (t_main)
    SDL_WaitThread(t_main, NULL);

  // if spice is still connected send key up events for any pressed keys
  if (params.useSpice && spice_ready())
  {
    for(int i = 0; i < SDL_NUM_SCANCODES; ++i)
      if (state.keyDown[i])
      {
        uint32_t scancode = mapScancode(i);
        if (scancode == 0)
          continue;
        state.keyDown[i] = false;
        spice_key_up(scancode);
      }

    if (t_spice)
      SDL_WaitThread(t_spice, NULL);

    spice_disconnect();
  }

  if (state.lgr)
    state.lgr->deinitialize(state.lgrData);

  if (state.window)
    SDL_DestroyWindow(state.window);

  if (cursor)
    SDL_FreeCursor(cursor);

  if (state.shm)
  {
    munmap(state.shm, state.shmSize);
    close(state.shmFD);
  }

  SDL_Quit();
  return 0;
}

void doHelp(char * app)
{
  char x[8], y[8];
  snprintf(x, sizeof(x), "%d", params.x);
  snprintf(y, sizeof(y), "%d", params.y);

  fprintf(stderr,
    "Looking Glass Client\n"
    "Usage: %s [OPTION]...\n"
    "Example: %s -h\n"
    "\n"
    "  -h        Print out this help\n"
    "\n"
    "  -C PATH   Specify an additional configuration file to load\n"
    "  -f PATH   Specify the path to the shared memory file [current: %s]\n"
    "  -L SIZE   Specify the size in MB of the shared memory file (0 = detect) [current: %d]\n"
    "\n"
    "  -s        Disable spice client\n"
    "  -c HOST   Specify the spice host or UNIX socket [current: %s]\n"
    "  -p PORT   Specify the spice port or 0 for UNIX socket [current: %d]\n"
    "  -j        Disable cursor position scaling\n"
    "  -M        Don't hide the host cursor\n"
    "\n"
    "  -K        Set the FPS limit [current: %d]\n"
    "  -k        Enable FPS display\n"
    "  -g NAME   Force the use of a specific renderer\n"
    "  -o OPTION Specify a renderer option (ie: opengl:vsync=0)\n"
    "            Alternatively specify \"list\" to list all renderers and their options\n"
    "\n"
    "  -a        Auto resize the window to the guest\n"
    "  -n        Don't allow the window to be manually resized\n"
    "  -r        Don't maintain the aspect ratio\n"
    "  -d        Borderless mode\n"
    "  -F        Borderless fullscreen mode\n"
    "  -x XPOS   Initial window X position [current: %s]\n"
    "  -y YPOS   Initial window Y position [current: %s]\n"
    "  -w WIDTH  Initial window width [current: %u]\n"
    "  -b HEIGHT Initial window height [current: %u]\n"
    "  -Q        Ignore requests to quit (ie: Alt+F4)\n"
    "  -S        Disable the screensaver\n"
    "  -G        Don't capture the keyboard in capture mode\n"
    "  -m CODE   Specify the capture key [current: %u (%s)]\n"
    "            See https://wiki.libsdl.org/SDLScancodeLookup for valid values\n"
    "  -q        Disable alert messages [current: %s]\n"
    "\n"
    "  -l        License information\n"
    "\n",
    app,
    app,
    params.shmFile,
    params.shmSize,
    params.spiceHost,
    params.spicePort,
    params.fpsLimit,
    params.center ? "center" : x,
    params.center ? "center" : y,
    params.w,
    params.h,
    params.captureKey,
    params.disableAlerts ? "disabled" : "enabled",
    SDL_GetScancodeName(params.captureKey)
  );
}

void doLicense()
{
  fprintf(stderr,
    "\n"
    "Looking Glass - KVM FrameRelay (KVMFR) Client\n"
    "Copyright(C) 2017 Geoffrey McRae <geoff@hostfission.com>\n"
    "https://looking-glass.hostfission.com\n"
    "\n"
    "This program is free software; you can redistribute it and / or modify it under\n"
    "the terms of the GNU General Public License as published by the Free Software\n"
    "Foundation; either version 2 of the License, or (at your option) any later\n"
    "version.\n"
    "\n"
    "This program is distributed in the hope that it will be useful, but WITHOUT ANY\n"
    "WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A\n"
    "PARTICULAR PURPOSE.See the GNU General Public License for more details.\n"
    "\n"
    "You should have received a copy of the GNU General Public License along with\n"
    "this program; if not, write to the Free Software Foundation, Inc., 59 Temple\n"
    "Place, Suite 330, Boston, MA 02111 - 1307 USA\n"
    "\n"
  );
}

static bool load_config(const char * configFile)
{
  config_t cfg;
  int itmp;
  const char *stmp;

  config_init(&cfg);
  if (!config_read_file(&cfg, configFile))
  {
    DEBUG_ERROR("Config file error %s:%d - %s",
      config_error_file(&cfg),
      config_error_line(&cfg),
      config_error_text(&cfg)
    );
    return false;
  }

  config_setting_t * global = config_lookup(&cfg, "global");
  if (global)
  {
    if (config_setting_lookup_string(global, "shmFile", &stmp))
    {
      free(params.shmFile);
      params.shmFile = strdup(stmp);
    }

    if (config_setting_lookup_int(global, "shmSize", &itmp))
      params.shmSize = itmp * 1024 * 1024;

    if (config_setting_lookup_string(global, "forceRenderer", &stmp))
    {
      bool ok = false;
      for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
        if (strcasecmp(LG_Renderers[i]->get_name(), stmp) == 0)
        {
          params.forceRenderer      = true;
          params.forceRendererIndex = i;
          ok = true;
          break;
        }

      if (!ok)
      {
        DEBUG_ERROR("No such renderer: %s", stmp);
        config_destroy(&cfg);
        return false;
      }
    }

    if (config_setting_lookup_bool(global, "scaleMouseInput" , &itmp)) params.scaleMouseInput  = (itmp != 0);
    if (config_setting_lookup_bool(global, "hideMouse"       , &itmp)) params.hideMouse        = (itmp != 0);
    if (config_setting_lookup_bool(global, "showFPS"         , &itmp)) params.showFPS          = (itmp != 0);
    if (config_setting_lookup_bool(global, "autoResize"      , &itmp)) params.autoResize       = (itmp != 0);
    if (config_setting_lookup_bool(global, "allowResize"     , &itmp)) params.allowResize      = (itmp != 0);
    if (config_setting_lookup_bool(global, "keepAspect"      , &itmp)) params.keepAspect       = (itmp != 0);
    if (config_setting_lookup_bool(global, "borderless"      , &itmp)) params.borderless       = (itmp != 0);
    if (config_setting_lookup_bool(global, "fullScreen"      , &itmp)) params.fullscreen       = (itmp != 0);
    if (config_setting_lookup_bool(global, "ignoreQuit"      , &itmp)) params.ignoreQuit       = (itmp != 0);
    if (config_setting_lookup_bool(global, "allowScreensaver", &itmp)) params.allowScreensaver = (itmp != 0);
    if (config_setting_lookup_bool(global, "disableAlerts"   , &itmp)) params.disableAlerts    = (itmp != 0);

    if (config_setting_lookup_int(global, "x", &params.x)) params.center = false;
    if (config_setting_lookup_int(global, "y", &params.y)) params.center = false;

    if (config_setting_lookup_int(global, "w", &itmp))
    {
      if (itmp < 1)
      {
        DEBUG_ERROR("Invalid window width, must be greater then 1px");
        config_destroy(&cfg);
        return false;
      }
      params.w = (unsigned int)itmp;
    }

    if (config_setting_lookup_int(global, "h", &itmp))
    {
      if (itmp < 1)
      {
        DEBUG_ERROR("Invalid window height, must be greater then 1px");
        config_destroy(&cfg);
        return false;
      }
      params.h = (unsigned int)itmp;
    }

    if (config_setting_lookup_int(global, "fpsLimit", &itmp))
    {
      if (itmp < 1)
      {
        DEBUG_ERROR("Invalid FPS limit, must be greater then 0");
        config_destroy(&cfg);
        return false;
      }
      params.fpsLimit = (unsigned int)itmp;
    }

    if (config_setting_lookup_int(global, "captureKey", &itmp))
    {
      if (itmp <= SDL_SCANCODE_UNKNOWN || itmp > SDL_SCANCODE_APP2)
      {
        DEBUG_ERROR("Invalid capture key value, see https://wiki.libsdl.org/SDLScancodeLookup");
        config_destroy(&cfg);
        return false;
      }
      params.captureKey = (SDL_Scancode)itmp;
    }
  }

  config_setting_t * spice = config_lookup(&cfg, "spice");
  if (spice)
  {
    if (config_setting_lookup_bool(spice, "use", &itmp))
      params.useSpice = (itmp != 0);

    if (config_setting_lookup_string(spice, "host", &stmp))
    {
      free(params.spiceHost);
      params.spiceHost = strdup(stmp);
    }

    if (config_setting_lookup_int(spice, "port", &itmp))
    {
      if (itmp < 0 || itmp > 65535)
      {
        DEBUG_ERROR("Invalid spice port");
        config_destroy(&cfg);
        return false;
      }
      params.spicePort = itmp;
    }
  }

  for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
  {
    const LG_Renderer * r     = LG_Renderers[i];
    RendererOpts      * opts  = &params.rendererOpts[i];
    config_setting_t  * group = config_lookup(&cfg, r->get_name());
    if (!group)
      continue;

    for(unsigned int j = 0; j < r->option_count; ++j)
    {
      const char * name = r->options[j].name;
      if (!config_setting_lookup_string(group, name, &stmp))
        continue;

      if (r->options[j].validator && !r->options[j].validator(stmp))
      {
        DEBUG_ERROR("Renderer \"%s\" reported invalid value for option \"%s\"", r->get_name(), name);
        config_destroy(&cfg);
        return false;
      }

      if (opts->argc == opts->size)
      {
        opts->size += 5;
        opts->argv  = realloc(opts->argv, sizeof(LG_RendererOptValue) * opts->size);
      }

      opts->argv[opts->argc].opt   = &r->options[j];
      opts->argv[opts->argc].value = strdup(stmp);
      ++opts->argc;
    }
  }

  config_destroy(&cfg);
  return true;
}

int main(int argc, char * argv[])
{
  params.shmFile   = strdup(params.shmFile  );
  params.spiceHost = strdup(params.spiceHost);

  {
    // load any global then local config options first
    struct stat st;
    if (stat("/etc/looking-glass.conf", &st) >= 0)
    {
      DEBUG_INFO("Loading config from: /etc/looking-glass.conf");
      if (!load_config("/etc/looking-glass.conf"))
        return -1;
    }

    struct passwd * pw = getpwuid(getuid());
    const char pattern[] = "%s/.looking-glass.conf";
    const size_t len = strlen(pw->pw_dir) + sizeof(pattern);
    char buffer[len];
    snprintf(buffer, len, pattern, pw->pw_dir);
    if (stat(buffer, &st) >= 0)
    {
      DEBUG_INFO("Loading config from: %s", buffer);
      if (!load_config(buffer))
        return -1;
    }
  }

  for(;;)
  {
    switch(getopt(argc, argv, "hC:f:L:sc:p:jMvK:kg:o:anrdFx:y:w:b:QSGm:lq"))
    {
      case '?':
      case 'h':
      default :
        doHelp(argv[0]);
        return -1;

      case -1:
        break;

      case 'C':
        params.configFile = optarg;
        if (!load_config(optarg))
          return -1;
        continue;

      case 'f':
        free(params.shmFile);
        params.shmFile = strdup(optarg);
        continue;

      case 'L':
        params.shmSize = atoi(optarg) * 1024 * 1024;
        continue;

      case 's':
        params.useSpice = false;
        continue;

      case 'c':
        free(params.spiceHost);
        params.spiceHost = strdup(optarg);
        continue;

      case 'p':
        params.spicePort = atoi(optarg);
        continue;

      case 'j':
        params.scaleMouseInput = false;
        continue;

      case 'M':
        params.hideMouse = false;
        continue;

      case 'K':
        params.fpsLimit = atoi(optarg);
        continue;

      case 'k':
        params.showFPS = true;
        continue;

      case 'g':
      {
        bool ok = false;
        for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
          if (strcasecmp(LG_Renderers[i]->get_name(), optarg) == 0)
          {
            params.forceRenderer      = true;
            params.forceRendererIndex = i;
            ok = true;
            break;
          }

        if (!ok)
        {
          fprintf(stderr, "No such renderer: %s\n", optarg);
          fprintf(stderr, "Use '-o list' obtain a list of options\n");
          doHelp(argv[0]);
          return -1;
        }

        continue;
      }

      case 'o':
      {
        if (strcasecmp(optarg, "list") == 0)
        {
          size_t maxLen = 0;
          for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
          {
            const LG_Renderer * r = LG_Renderers[i];
            for(unsigned int j = 0; j < r->option_count; ++j)
            {
              const size_t len = strlen(r->options[j].name);
              if (len > maxLen)
                maxLen = len;
            }
          }

          fprintf(stderr, "\nRenderer Option List\n");
          for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
          {
            const LG_Renderer * r = LG_Renderers[i];
            fprintf(stderr, "\n%s\n", r->get_name());
            for(unsigned int j = 0; j < r->option_count; ++j)
            {
              const size_t pad = maxLen - strlen(r->options[j].name);
              for(int i = 0; i < pad; ++i)
                fputc(' ', stderr);

              fprintf(stderr, "  %s - %s\n", r->options[j].name, r->options[j].desc);
            }
          }
          fprintf(stderr, "\n");
          return -1;
        }

        const LG_Renderer  * renderer = NULL;
        RendererOpts       * opts     = NULL;

        const size_t len  = strlen(optarg);
        const char * name = strtok(optarg, ":");

        for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
          if (strcasecmp(LG_Renderers[i]->get_name(), name) == 0)
          {
            renderer = LG_Renderers[i];
            opts     = &params.rendererOpts[i];
            break;
          }

        if (!renderer)
        {
          fprintf(stderr, "No such renderer: %s\n", name);
          doHelp(argv[0]);
          return -1;
        }

        const char * option = strtok(NULL  , "=");
        if (!option)
        {
          fprintf(stderr, "Renderer option name not specified\n");
          doHelp(argv[0]);
          return -1;
        }

        const LG_RendererOpt * opt = NULL;
        for(unsigned int i = 0; i < renderer->option_count; ++i)
          if (strcasecmp(option, renderer->options[i].name) == 0)
          {
            opt = &renderer->options[i];
            break;
          }

        if (!opt)
        {
          fprintf(stderr, "Renderer \"%s\" doesn't have the option: %s\n", renderer->get_name(), option);
          doHelp(argv[0]);
          return -1;
        }

        const char * value = NULL;
        if (len > strlen(name) + strlen(option) + 2)
          value = option + strlen(option) + 1;

        if (opt->validator && !opt->validator(value))
        {
          fprintf(stderr, "Renderer \"%s\" reported invalid value for option \"%s\"\n", renderer->get_name(), option);
          doHelp(argv[0]);
          return -1;
        }

        if (opts->argc == opts->size)
        {
          opts->size += 5;
          opts->argv  = realloc(opts->argv, sizeof(LG_RendererOptValue) * opts->size);
        }

        opts->argv[opts->argc].opt   = opt;
        opts->argv[opts->argc].value = strdup(value);
        ++opts->argc;
        continue;
      }

      case 'a':
        params.autoResize = true;
        continue;

      case 'n':
        params.allowResize = false;
        continue;

      case 'r':
        params.keepAspect = false;
        continue;

      case 'd':
        params.borderless = true;
        continue;

      case 'F':
        params.fullscreen = true;
        continue;

      case 'x':
        params.center = false;
        params.x = atoi(optarg);
        continue;

      case 'y':
        params.center = false;
        params.y = atoi(optarg);
        continue;

      case 'w':
        params.w = atoi(optarg);
        continue;

      case 'b':
        params.h = atoi(optarg);
        continue;

      case 'Q':
        params.ignoreQuit = true;
        continue;

      case 'S':
        params.allowScreensaver = false;
        continue;

      case 'G':
        params.grabKeyboard = false;
        continue;

      case 'm':
        params.captureKey = atoi(optarg);
        continue;

      case 'q':
        params.disableAlerts = true;
        continue;

      case 'l':
        doLicense();
        return 0;
    }
    break;
  }

  if (optind != argc)
  {
    fprintf(stderr, "A non option was supplied\n");
    doHelp(argv[0]);
    return -1;
  }

  if (params.grabKeyboard)
  {
    SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
  }

  const int ret = run();

  free(params.shmFile);
  free(params.spiceHost);
  for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
  {
    RendererOpts * opts = &params.rendererOpts[i];
    for(unsigned int j = 0; j < opts->argc; ++j)
      free(opts->argv[j].value);
    free(opts->argv);
  }

  return ret;
}