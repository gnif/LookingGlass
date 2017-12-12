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
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <fontconfig/fontconfig.h>

#include "debug.h"
#include "utils.h"
#include "KVMFR.h"
#include "ivshmem/ivshmem.h"
#include "spice/spice.h"
#include "kb.h"

#include "lg-renderers.h"

struct AppState
{
  bool      running;
  bool      started;

  TTF_Font       *font;
  SDL_Point       srcSize;
  LG_RendererRect dstRect;
  SDL_Point       cursor;
  float           scaleX, scaleY;

  const LG_Renderer * lgr ;
  void              * lgrData;

  SDL_Window         * window;
  struct KVMFRHeader * shm;
  unsigned int         shmSize;
};

struct AppParams
{
  bool         autoResize;
  bool         allowResize;
  bool         keepAspect;
  bool         borderless;
  bool         center;
  int          x, y;
  unsigned int w, h;
  const char * ivshmemSocket;
  bool         useMipmap;
  bool         showFPS;
  bool         useSpice;
  const char * spiceHost;
  unsigned int spicePort;
  bool         scaleMouseInput;
  bool         hideMouse;
};

struct AppState  state;
struct AppParams params =
{
  .autoResize       = false,
  .allowResize      = true,
  .keepAspect       = true,
  .borderless       = false,
  .center           = true,
  .x                = 0,
  .y                = 0,
  .w                = 1024,
  .h                = 768,
  .ivshmemSocket    = "/tmp/ivshmem_socket",
  .useMipmap        = true,
  .showFPS          = false,
  .useSpice         = true,
  .spiceHost        = "127.0.0.1",
  .spicePort        = 5900,
  .scaleMouseInput  = true,
  .hideMouse        = true
};

inline void updatePositionInfo()
{
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

  if (state.lgr)
    state.lgr->on_resize(state.lgrData, w, h, state.dstRect);
}

int renderThread(void * unused)
{
  bool                error = false;
  struct KVMFRHeader  header;
  volatile uint32_t * updateCount = &state.shm->updateCount;

  ivshmem_kick_irq(state.shm->guestID, 0);
  while(state.running)
  {
    // poll until we have a new frame, or we time out
    while(header.updateCount == *updateCount && state.running) {
      const struct timespec s = {
        .tv_sec  = 0,
        .tv_nsec = 1000
      };
      nanosleep(&s, NULL);
    }

    if (!state.running)
      break;

    // we must take a copy of the header, both to let the guest advance and to
    // prevent the contained arguments being abused to overflow buffers
    memcpy(&header, state.shm, sizeof(struct KVMFRHeader));
    if (!ivshmem_kick_irq(header.guestID, 0))
    {
      usleep(1000);
      continue;
    }

    // check the header's magic and version are valid
    if (
      memcmp(header.magic, KVMFR_HEADER_MAGIC, sizeof(KVMFR_HEADER_MAGIC)) != 0 ||
      header.version != KVMFR_HEADER_VERSION
    ){
      usleep(1000);
      continue;
    }

    // if we have a frame
    if (header.flags & KVMFR_HEADER_FLAG_FRAME)
    {
      // sainty check of the frame format
      if (
        header.frame.type    >= FRAME_TYPE_MAX ||
        header.frame.width   == 0 ||
        header.frame.height  == 0 ||
        header.frame.stride  == 0 ||
        header.frame.dataPos == 0 ||
        header.frame.dataPos > state.shmSize
      ){
        usleep(1000);
        continue;
      }

      // setup the renderer format with the frame format details
      LG_RendererFormat lgrFormat;
      lgrFormat.width  = header.frame.width;
      lgrFormat.height = header.frame.height;
      lgrFormat.stride = header.frame.stride;

      switch(header.frame.type)
      {
        case FRAME_TYPE_ARGB:
          lgrFormat.pitch = header.frame.stride * 4;
          lgrFormat.bpp   = 32;
          break;

        case FRAME_TYPE_RGB:
          lgrFormat.pitch = header.frame.stride * 3;
          lgrFormat.bpp   = 24;
          break;

        default:
          DEBUG_ERROR("Unsupported frameType");
          error = true;
          break;
      }

      if (error)
        break;

      // check the header's dataPos is sane
      const size_t dataSize = lgrFormat.height * lgrFormat.pitch;
      if (header.frame.dataPos + dataSize > state.shmSize)
      {
        DEBUG_ERROR("The guest sent an invalid dataPos");
        break;
      }

      // check if we have a compatible renderer
      if (!state.lgr || !state.lgr->is_compatible(state.lgrData, lgrFormat))
      {
        int width, height;
        SDL_GetWindowSize(state.window, &width, &height);

        LG_RendererParams lgrParams;
        lgrParams.window  = state.window;
        lgrParams.font    = state.font;
        lgrParams.showFPS = params.showFPS;
        lgrParams.width   = width;
        lgrParams.height  = height;

        DEBUG_INFO("Data Format: w=%u, h=%u, s=%u, p=%u, bpp=%u",
            lgrFormat.width, lgrFormat.height, lgrFormat.stride, lgrFormat.pitch, lgrFormat.bpp);

        // first try to reinitialize any existing renderer
        if (state.lgr)
        {
          state.lgr->deinitialize(state.lgrData);
          if (state.lgr->initialize(&state.lgrData, lgrParams, lgrFormat))
          {
            DEBUG_INFO("Reinitialized %s", state.lgr->get_name());
          }
          else
          {
            DEBUG_ERROR("Failed to reinitialize %s, trying other renderers", state.lgr->get_name());
            state.lgr->deinitialize(state.lgrData);
            state.lgr = NULL;
          }
        }

        if (!state.lgr)
        {
          // probe for a a suitable renderer
          for(const LG_Renderer **r = &LG_Renderers[0]; *r; ++r)
          {
            if (!IS_LG_RENDERER_VALID(*r))
            {
              DEBUG_ERROR("FIXME: Renderer %d is invalid, skipping", (int)(r - &LG_Renderers[0]));
              continue;
            }

            state.lgrData = NULL;
            if (!(*r)->initialize(&state.lgrData, lgrParams, lgrFormat))
            {
              (*r)->deinitialize(state.lgrData);
              continue;
            }

            state.lgr = *r;
            DEBUG_INFO("Initialized %s", (*r)->get_name());
            break;
          }

          if (!state.lgr)
          {
            DEBUG_INFO("Unable to find a suitable renderer");
            return -1;
          }
        }

        state.srcSize.x = header.frame.width;
        state.srcSize.y = header.frame.height;
        if (params.autoResize)
          SDL_SetWindowSize(state.window, header.frame.width, header.frame.height);
        updatePositionInfo();
      }

      const uint8_t * data = (const uint8_t *)state.shm + header.frame.dataPos;
      if (!state.lgr->on_frame_event(state.lgrData, data, params.useMipmap))
      {
        DEBUG_ERROR("Failed to render the frame");
        break;
      }
    }

    // if we have cursor data
    if (header.flags & KVMFR_HEADER_FLAG_CURSOR)
    {
      if (header.cursor.flags & KVMFR_CURSOR_FLAG_POS)
      {
        state.cursor.x = header.cursor.x;
        state.cursor.y = header.cursor.y;
      }

      if (header.cursor.flags & KVMFR_CURSOR_FLAG_SHAPE)
      {
        LG_RendererCursor c = LG_CURSOR_COLOR;
        switch(header.cursor.type)
        {
          case CURSOR_TYPE_COLOR       : c = LG_CURSOR_COLOR       ; break;
          case CURSOR_TYPE_MONOCHROME  : c = LG_CURSOR_MONOCHROME  ; break;
          case CURSOR_TYPE_MASKED_COLOR: c = LG_CURSOR_MASKED_COLOR; break;
          default:
            DEBUG_ERROR("Invalid cursor type");
            break;
        }

        if (state.lgr)
        {
          if (!state.lgr->on_mouse_shape(
            state.lgrData,
            c,
            header.cursor.w,
            header.cursor.h,
            header.cursor.pitch,
            header.cursor.shape
          ))
          {
            DEBUG_ERROR("Failed to update mouse shape");
            break;
          }
        }
      }

      if (state.lgr)
      {
        state.lgr->on_mouse_event(
          state.lgrData,
          (header.cursor.flags & KVMFR_CURSOR_FLAG_VISIBLE) != 0,
          state.cursor.x,
          state.cursor.y
        );
      }
    }

    if (state.lgr)
      state.lgr->render(state.lgrData);
  }

  if (state.lgr)
    state.lgr->deinitialize(state.lgrData);

  return 0;
}

int ivshmemThread(void * arg)
{
  while(state.running)
    if (!ivshmem_process())
    {
      if (state.running)
      {
        state.running = false;
        DEBUG_ERROR("failed to process ivshmem messages");
      }
      break;
    }

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

  spice_disconnect();
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

int eventThread(void * arg)
{
  bool serverMode   = false;
  bool realignGuest = true;
  bool keyDown[SDL_NUM_SCANCODES];

  memset(keyDown, 0, sizeof(keyDown));

  // ensure mouse acceleration is identical in server mode
  SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "1", SDL_HINT_OVERRIDE);

  while(state.running)
  {
    SDL_Event event;
    if (!SDL_PollEvent(&event))
    {
      usleep(1000);
      continue;
    }

    if (event.type == SDL_QUIT)
    {
      state.running = false;
      break;
    }

    if (!params.useSpice)
      continue;

    switch(event.type)
    {
      case SDL_KEYDOWN:
      {
        SDL_Scancode sc = event.key.keysym.scancode;
        if (sc == SDL_SCANCODE_SCROLLLOCK)
        {
          if (event.key.repeat)
            break;

          serverMode = !serverMode;
          spice_mouse_mode(serverMode);
          SDL_SetRelativeMouseMode(serverMode);

          if (!serverMode)
            realignGuest = true;
          break;
        }

        uint32_t scancode = mapScancode(sc);
        if (scancode == 0)
          break;

        if (spice_key_down(scancode))
          keyDown[sc] = true;
        else
        {
          DEBUG_ERROR("SDL_KEYDOWN: failed to send message");
          break;
        }
        break;
      }

      case SDL_KEYUP:
      {
        SDL_Scancode sc = event.key.keysym.scancode;
        if (sc == SDL_SCANCODE_SCROLLLOCK)
          break;

        // avoid sending key up events when we didn't send a down
        if (!keyDown[sc])
          break;

        uint32_t scancode = mapScancode(sc);
        if (scancode == 0)
          break;

        if (spice_key_up(scancode))
          keyDown[sc] = false;
        else
        {
          DEBUG_ERROR("SDL_KEYUP: failed to send message");
          break;
        }
        break;
      }

      case SDL_MOUSEWHEEL:
        if (
          !spice_mouse_press  (event.wheel.y == 1 ? 4 : 5) ||
          !spice_mouse_release(event.wheel.y == 1 ? 4 : 5)
          )
        {
          DEBUG_ERROR("SDL_MOUSEWHEEL: failed to send messages");
          break;
        }
        break;

      case SDL_MOUSEMOTION:
      {
        if (
          !serverMode && (
            event.motion.x < state.dstRect.x                   ||
            event.motion.x > state.dstRect.x + state.dstRect.w ||
            event.motion.y < state.dstRect.y                   ||
            event.motion.y > state.dstRect.y + state.dstRect.h
          )
        )
        {
          realignGuest = true;
          break;
        }

        int x = 0;
        int y = 0;
        if (realignGuest)
        {
          x = event.motion.x - state.dstRect.x;
          y = event.motion.y - state.dstRect.y;
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

        x = event.motion.xrel;
        y = event.motion.yrel;
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

      case SDL_MOUSEBUTTONDOWN:
        if (
          !spice_mouse_position(event.button.x, event.button.y) ||
          !spice_mouse_press(event.button.button)
        )
        {
          DEBUG_ERROR("SDL_MOUSEBUTTONDOWN: failed to send message");
          break;
        }
        break;

      case SDL_MOUSEBUTTONUP:
        if (
          !spice_mouse_position(event.button.x, event.button.y) ||
          !spice_mouse_release(event.button.button)
        )
        {
          DEBUG_ERROR("SDL_MOUSEBUTTONUP: failed to send message");
          break;
        }
        break;

      case SDL_WINDOWEVENT:
      {
        switch(event.window.event)
        {
          case SDL_WINDOWEVENT_ENTER:
            realignGuest = true;
            break;

          case SDL_WINDOWEVENT_SIZE_CHANGED:
            updatePositionInfo();
            realignGuest = true;
            break;
        }
        break;
      }

      default:
        break;
    }
  }

  return 0;
}

int run()
{
  DEBUG_INFO("Looking Glass (" BUILD_VERSION ")");

  memset(&state, 0, sizeof(state));
  state.running = true;
  state.scaleX  = 1.0f;
  state.scaleY  = 1.0f;

  if (SDL_Init(SDL_INIT_VIDEO) < 0)
  {
    DEBUG_ERROR("SDL_Init Failed");
    return -1;
  }

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

  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 0);
  state.window = SDL_CreateWindow(
    "Looking Glass (Client)",
    params.center ? SDL_WINDOWPOS_CENTERED : params.x,
    params.center ? SDL_WINDOWPOS_CENTERED : params.y,
    params.w,
    params.h,
    (
      SDL_WINDOW_SHOWN  |
      SDL_WINDOW_OPENGL |
      (params.allowResize ? SDL_WINDOW_RESIZABLE  : 0) |
      (params.borderless  ? SDL_WINDOW_BORDERLESS : 0)
    )
  );

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

  int         shm_fd    = 0;
  SDL_Thread *t_ivshmem = NULL;
  SDL_Thread *t_spice   = NULL;
  SDL_Thread *t_event   = NULL;

  while(1)
  {
    if (!ivshmem_connect(params.ivshmemSocket))
    {
      DEBUG_ERROR("failed to connect to the ivshmem server");
      break;
    }

    if (!(t_ivshmem = SDL_CreateThread(ivshmemThread, "ivshmemThread", NULL)))
    {
      DEBUG_ERROR("ivshmem create thread failed");
      break;
    }

    state.shm = (struct KVMFRHeader *)ivshmem_get_map();
    if (!state.shm)
    {
      DEBUG_ERROR("Failed to map memory");
      break;
    }
    state.shmSize     = ivshmem_get_map_size();
    state.shm->hostID = ivshmem_get_id();

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

    if (!(t_event = SDL_CreateThread(eventThread, "eventThread", NULL)))
    {
      DEBUG_ERROR("gpu create thread failed");
      break;
    }

    while(state.running)
      renderThread(NULL);

    break;
  }

  state.running = false;

  if (t_event)
    SDL_WaitThread(t_event, NULL);

  // this needs to happen here to abort any waiting reads
  // as ivshmem uses recvmsg which has no timeout
  ivshmem_disconnect();
  if (t_ivshmem)
    SDL_WaitThread(t_ivshmem, NULL);

  if (t_spice)
    SDL_WaitThread(t_spice, NULL);

  if (state.window)
    SDL_DestroyWindow(state.window);

  if (cursor)
    SDL_FreeCursor(cursor);

  if (shm_fd)
    close(shm_fd);

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
    "  -f PATH   Specify the path to the ivshmem socket [current: %s]\n"
    "\n"
    "  -s        Disable spice client\n"
    "  -c HOST   Specify the spice host [current: %s]\n"
    "  -p PORT   Specify the spice port [current: %d]\n"
    "  -j        Disable cursor position scaling\n"
    "  -M        Don't hide the host cursor\n"
    "\n"
    "  -m        Disable mipmapping\n"
    "  -v        Disable VSync\n"
    "  -k        Enable FPS display\n"
    "\n"
    "  -a        Auto resize the window to the guest\n"
    "  -n        Don't allow the window to be manually resized\n"
    "  -r        Don't maintain the aspect ratio\n"
    "  -d        Borderless mode\n"
    "  -x XPOS   Initial window X position [current: %s]\n"
    "  -y YPOS   Initial window Y position [current: %s]\n"
    "  -w WIDTH  Initial window width [current: %u]\n"
    "  -b HEIGHT Initial window height [current: %u]\n"
    "\n"
    "  -l        License information\n"
    "\n",
    app,
    app,
    params.ivshmemSocket,
    params.spiceHost,
    params.spicePort,
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

int main(int argc, char * argv[])
{
  int c;
  while((c = getopt(argc, argv, "hf:sc:p:jMmkanrdx:y:w:b:l")) != -1)
    switch(c)
    {
      case '?':
      case 'h':
      default :
        doHelp(argv[0]);
        return (c == 'h') ? 0 : -1;

      case 'f':
        params.ivshmemSocket = optarg;
        break;

      case 's':
        params.useSpice = false;
        break;

      case 'c':
        params.spiceHost = optarg;
        break;

      case 'p':
        params.spicePort = atoi(optarg);
        break;

      case 'j':
        params.scaleMouseInput = false;
        break;

      case 'M':
        params.hideMouse = false;
        break;

      case 'm':
        params.useMipmap = false;
        break;

      case 'k':
        params.showFPS = true;
        break;

      case 'a':
        params.autoResize = true;
        break;

      case 'n':
        params.allowResize = false;
        break;

      case 'r':
        params.keepAspect = false;
        break;

      case 'd':
        params.borderless = true;
        break;

      case 'x':
        params.center = false;
        params.x = atoi(optarg);
        break;

      case 'y':
        params.center = false;
        params.y = atoi(optarg);
        break;

      case 'w':
        params.w = atoi(optarg);
        break;

      case 'b':
        params.h = atoi(optarg);
        break;

      case 'l':
        doLicense();
        return 0;
    }

  return run();
}