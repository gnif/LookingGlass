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
#include <SDL2/SDL_ttf.h>
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
#include <fontconfig/fontconfig.h>

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

  TTF_Font           * font;
  SDL_Point            srcSize;
  LG_RendererRect      dstRect;
  SDL_Point            cursor;
  bool                 cursorVisible;
  bool                 haveCursorPos;
  float                scaleX, scaleY;

  const LG_Renderer  * lgr ;
  void               * lgrData;

  SDL_Window         * window;
  int                  shmFD;
  struct KVMFRHeader * shm;
  unsigned int         shmSize;
  int64_t              fpsSleep;
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
  .forceRenderer    = false
};

static inline void updatePositionInfo()
{
  if (!state.started)
    return;

  int w, h;
  SDL_GetWindowSize(state.window, &w, &h);

  if (params.keepAspect)
  {
    const float srcAspect = (float)state.srcSize.y / (float)state.srcSize.x;
    const float wndAspect = (float)h / (float)w;
    if (wndAspect < srcAspect)
    {
      state.dstRect.w = (float)h / srcAspect;
      state.dstRect.h = h;
      state.dstRect.x = (w >> 1) - (state.dstRect.w >> 1);
      state.dstRect.y = 0;
    }
    else
    {
      state.dstRect.w = w;
      state.dstRect.h = (float)w * srcAspect;
      state.dstRect.x = 0;
      state.dstRect.y = (h >> 1) - (state.dstRect.h >> 1);
    }
  }
  else
  {
    state.dstRect.x = 0;
    state.dstRect.y = 0;
    state.dstRect.w = w;
    state.dstRect.h = h;
  }

  state.scaleX = (float)state.srcSize.y / (float)state.dstRect.h;
  state.scaleY = (float)state.srcSize.x / (float)state.dstRect.w;

  DEBUG_INFO("client %dx%d, guest %dx%d, target %dx%d, scaleX: %.2f, scaleY %.2f",
    w, h,
    state.srcSize.x, state.srcSize.y,
    state.dstRect.w, state.dstRect.h,
    state.scaleX   , state.scaleY
  );

  if (w != state.srcSize.x || h != state.srcSize.y)
    DEBUG_WARN("Window size doesn't match guest resolution, cursor alignment may not be reliable");

  if (state.lgr)
    state.lgr->on_resize(state.lgrData, w, h, state.dstRect);
}

int renderThread(void * unused)
{
  while(state.running)
  {
    const uint64_t start = microtime();

    if (!state.lgr->render(state.lgrData, state.window))
      break;

    const uint64_t total = microtime() - start;
    if (total < state.fpsSleep)
    {
      usleep(state.fpsSleep - total);
      int64_t delta   = (1000000 / params.fpsLimit) - (microtime() - start);
      state.fpsSleep += delta / 16;
      if (state.fpsSleep < 0)
        state.fpsSleep = 0;
    }
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

      usleep(1000);
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

    if (header.flags & KVMFR_CURSOR_FLAG_POS)
    {
      state.cursor.x      = header.x;
      state.cursor.y      = header.y;
      state.cursorVisible = header.flags & KVMFR_CURSOR_FLAG_VISIBLE;
      state.haveCursorPos = true;

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

  while(state.running)
  {
    // poll until we have a new frame
    if(!(state.shm->frame.flags & KVMFR_FRAME_FLAG_UPDATE))
    {
      if (!state.running)
        break;

      // allow for a maximum refresh of 400fps (1000/400 = 2.5ms), this should
      // befreqent enough without chewing up too much CPU time
      usleep(2500);
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
      usleep(1000);
      continue;
    }

    // setup the renderer format with the frame format details
    LG_RendererFormat lgrFormat;
    lgrFormat.width  = header.width;
    lgrFormat.height = header.height;
    lgrFormat.stride = header.stride;
    lgrFormat.pitch  = header.pitch;

    size_t dataSize;
    switch(header.type)
    {
      case FRAME_TYPE_ARGB:
        dataSize       = lgrFormat.height * lgrFormat.pitch;
        lgrFormat.comp = LG_COMPRESSION_NONE;
        lgrFormat.bpp  = 32;
        break;

      case FRAME_TYPE_H264:
        dataSize       = lgrFormat.pitch;
        lgrFormat.comp = LG_COMPRESSION_H264;
        lgrFormat.bpp  = 0;
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

  if (event->type == SDL_WINDOWEVENT)
  {
    switch(event->window.event)
    {
      case SDL_WINDOWEVENT_ENTER:
        realignGuest = true;
        break;

      case SDL_WINDOWEVENT_SIZE_CHANGED:
        updatePositionInfo();
        realignGuest = true;
        break;
    }
    return 0;
  }

  if (!params.useSpice)
    return 1;

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
        if (params.scaleMouseInput)
        {
          x = (float)x * state.scaleX;
          y = (float)y * state.scaleY;
        }
        x -= state.cursor.x;
        y -= state.cursor.y;
        realignGuest = false;

        if (!spice_mouse_motion(x, y))
          DEBUG_ERROR("SDL_MOUSEMOTION: failed to send message");
        break;
      }

      x = event->motion.xrel;
      y = event->motion.yrel;
      if (x != 0 || y != 0)
      {
        if (params.scaleMouseInput)
        {
          x = (float)x * state.scaleX;
          y = (float)y * state.scaleY;
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
      if (sc == SDL_SCANCODE_SCROLLLOCK)
      {
        if (event->key.repeat)
          break;

        serverMode = !serverMode;
        spice_mouse_mode(serverMode);
        SDL_SetRelativeMouseMode(serverMode);
        DEBUG_INFO("Server Mode: %s", serverMode ? "on" : "off");

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
      if (sc == SDL_SCANCODE_SCROLLLOCK)
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

    default:
      return 1;
  }

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
  state.running  = true;
  state.scaleX   = 1.0f;
  state.scaleY   = 1.0f;
  state.fpsSleep = 1000000 / params.fpsLimit;

  if (SDL_Init(SDL_INIT_VIDEO) < 0)
  {
    DEBUG_ERROR("SDL_Init Failed");
    return -1;
  }

  // override SDL's SIGINIT handler so that we can tell the difference between
  // SIGINT and the user sending a close event, such as ALT+F4
  signal(SIGINT, intHandler);

  if (params.showFPS)
  {
    if (TTF_Init() < 0)
    {
      DEBUG_ERROR("TTL_Init Failed");
      return -1;
    }

    FcConfig  * config = FcInitLoadConfigAndFonts();
    if (!config)
    {
      DEBUG_ERROR("FcInitLoadConfigAndFonts Failed");
      return -1;
    }

    FcPattern * pat = FcNameParse((const FcChar8*)"FreeMono");
    FcConfigSubstitute (config, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result;
    FcChar8 * file = NULL;
    FcPattern * font = FcFontMatch(config, pat, &result);

    if (font && (FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch))
    {
      state.font = TTF_OpenFont((char *)file, 14);
      if (!state.font)
      {
        DEBUG_ERROR("TTL_OpenFont Failed");
        return -1;
      }
    }
    else
    {
      DEBUG_ERROR("Failed to locate a font for FPS display");
      return -1;
    }
    FcPatternDestroy(pat);
  }

  LG_RendererParams lgrParams;
  lgrParams.font     = state.font;
  lgrParams.showFPS  = params.showFPS;
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

  if (params.ignoreQuit)
    SDL_SetHint(SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4, "1");

  if (params.fullscreen)
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

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

    // start the renderThread so we don't just display junk
    if (!(t_render = SDL_CreateThread(renderThread, "renderThread", NULL)))
    {
      DEBUG_ERROR("render create thread failed");
      break;
    }

    // flag the host that we are starting up this is important so that
    // the host wakes up if it is waiting on an interrupt, the host will
    // also send us the current mouse shape since we won't know it yet
    DEBUG_INFO("Waiting for host to signal it's ready...");
    __sync_or_and_fetch(&state.shm->flags, KVMFR_HEADER_FLAG_RESTART);

    while(state.running && (state.shm->flags & KVMFR_HEADER_FLAG_RESTART))
    {
      SDL_Event event;
      while(SDL_PollEvent(&event))
      {
        if (event.type == SDL_QUIT)
        {
          state.running = false;
          break;
        }
      }
      usleep(1000);
    }

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

    while(state.running)
    {
      SDL_Event event;
      while(SDL_PollEvent(&event))
      {
        if (event.type == SDL_QUIT)
        {
          if (!params.ignoreQuit)
            state.running = false;
          break;
        }
      }
      usleep(1000);
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

  TTF_Quit();
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
    params.h
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

    if (config_setting_lookup_bool(global, "scaleMouseInput", &itmp)) params.scaleMouseInput = (itmp != 0);
    if (config_setting_lookup_bool(global, "hideMouse"      , &itmp)) params.hideMouse       = (itmp != 0);
    if (config_setting_lookup_bool(global, "showFPS"        , &itmp)) params.showFPS         = (itmp != 0);
    if (config_setting_lookup_bool(global, "autoResize"     , &itmp)) params.autoResize      = (itmp != 0);
    if (config_setting_lookup_bool(global, "allowResize"    , &itmp)) params.allowResize     = (itmp != 0);
    if (config_setting_lookup_bool(global, "keepAspect"     , &itmp)) params.keepAspect      = (itmp != 0);
    if (config_setting_lookup_bool(global, "borderless"     , &itmp)) params.borderless      = (itmp != 0);
    if (config_setting_lookup_bool(global, "fullScreen"     , &itmp)) params.fullscreen      = (itmp != 0);
    if (config_setting_lookup_bool(global, "ignoreQuit"     , &itmp)) params.ignoreQuit      = (itmp != 0);

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
    switch(getopt(argc, argv, "hC:f:L:sc:p:jMvK:kg:o:anrdFx:y:w:b:Ql"))
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