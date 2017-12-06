/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>

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
#include <SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glu.h>

#include "debug.h"
#include "delay.h"
#include "memcpySSE.h"
#include "KVMFR.h"
#include "ivshmem/ivshmem.h"
#include "spice/spice.h"
#include "kb.h"

#include "lg-renderers.h"

#define VBO_BUFFERS 2

struct AppState
{
  bool      hasBufferStorage;

  bool      running;
  bool      started;

  TTF_Font       *font;
  SDL_Rect        srcRect;
  LG_RendererRect dstRect;
  float           scaleX, scaleY;

  SDL_Window         * window;
  SDL_Renderer       * renderer;
  struct KVMFRHeader * shm;
  unsigned int         shmSize;
};

struct AppParams
{
  bool         vsync;
  bool         autoResize;
  bool         allowResize;
  bool         keepAspect;
  bool         borderless;
  bool         center;
  int          x, y;
  unsigned int w, h;
  const char * ivshmemSocket;
  bool         useBufferStorage;
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
  .vsync            = true,
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
  .useBufferStorage = true,
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
  SDL_GetRendererOutputSize(state.renderer, &w, &h);

  if (params.keepAspect)
  {
    const float srcAspect = (float)state.srcRect.h / (float)state.srcRect.w;
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

  state.scaleX = (float)state.srcRect.h / (float)state.dstRect.h;
  state.scaleY = (float)state.srcRect.w / (float)state.dstRect.w;
}

inline bool areFormatsSame(const struct KVMFRHeader s1, const struct KVMFRHeader s2)
{
  return
    (s1.frameType != FRAME_TYPE_INVALID) &&
    (s2.frameType != FRAME_TYPE_INVALID) &&
    (s1.version   == s2.version  ) &&
    (s1.frameType == s2.frameType) &&
    (s1.width     == s2.width    ) &&
    (s1.height    == s2.height   );
}

inline uint64_t microtime()
{
  struct timeval time;
  gettimeofday(&time, NULL);
  return ((uint64_t)time.tv_sec * 1000000) + time.tv_usec;
}

int renderThread(void * unused)
{
  bool                error = false;
  struct KVMFRHeader  header;
  const LG_Renderer * lgr = NULL;
  void              * lgrData;
  unsigned int        frameCount = 0;
  SDL_Texture       * textTexture = NULL;
  SDL_Rect            textRect;

  uint64_t            waitTime    = 0;
  uint64_t            presentTime = 0;
  uint64_t            fpsTime     = 0;

  while(state.running)
  {
    glFlush();

    if (waitTime < 30000 && waitTime > 2000)
      usleep(waitTime - 2000);

    // poll for a new frame
    while(state.running && header.dataPos == state.shm->dataPos)
    {
      // if the frame is overdue
      if (microtime() - presentTime > waitTime)
      {
        enum IVSHMEMWaitResult result = ivshmem_wait_irq(0, (1000/30));
        if (result == IVSHMEM_WAIT_RESULT_OK)
          continue;

        if (result == IVSHMEM_WAIT_RESULT_TIMEOUT)
          break;

        if (result == IVSHMEM_WAIT_RESULT_ERROR)
        {
          DEBUG_ERROR("error during wait for host");
          state.running = false;
          break;
        }
      }
    }

    if (!state.running)
      break;

    // normally you would never put this into an OpenGL application but because
    // of our hybrid sleep/poll logic above the GPU has had time to finish up
    // anyway. Having this here ensures that the GPU doesn't buffer up
    // additional frames if the guest starts to outpace us.
    glFinish();

    waitTime = microtime() - presentTime;

    // we must take a copy of the header, both to let the guest advance and to
    // prevent the contained arguments being abused to overflow buffers
    memcpy(&header, state.shm, sizeof(struct KVMFRHeader));
    ivshmem_kick_irq(header.guestID, 0);

    // check the header's magic and version are valid
    if (
      memcmp(header.magic, KVMFR_HEADER_MAGIC, sizeof(KVMFR_HEADER_MAGIC)) != 0 ||
      header.version != KVMFR_HEADER_VERSION
    )
    {
      usleep(1000);
      continue;
    }

    // setup the renderer format with the frame format details
    LG_RendererFormat lgrFormat;
    lgrFormat.width  = header.width;
    lgrFormat.height = header.height;
    lgrFormat.stride = header.stride;

    switch(header.frameType)
    {
      case FRAME_TYPE_ARGB:
        lgrFormat.pitch = header.stride * 4;
        lgrFormat.bpp   = 32;
        break;

      case FRAME_TYPE_RGB:
        lgrFormat.pitch = header.stride * 3;
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
    if (header.dataPos + dataSize > state.shmSize)
    {
      DEBUG_ERROR("The guest sent an invalid dataPos");
      break;
    }

    // check if we have a compatible renderer
    if (!lgr || !lgr->is_compatible(lgrData, lgrFormat))
    {
      LG_RendererParams lgrParams;
      lgrParams.window   = state.window;
      lgrParams.renderer = state.renderer;

      DEBUG_INFO("Data Format: w=%u, h=%u, s=%u, p=%u, bpp=%u",
          lgrFormat.width, lgrFormat.height, lgrFormat.stride, lgrFormat.pitch, lgrFormat.bpp);

      // first try to reinitialize any existing renderer
      if (lgr)
      {
        lgr->deinitialize(lgrData);
        if (lgr->initialize(&lgrData, lgrParams, lgrFormat))
        {
          DEBUG_INFO("Reinitialized %s", lgr->get_name());
        }
        else
        {
          DEBUG_ERROR("Failed to reinitialize %s, trying other renderers", lgr->get_name());
          lgr->deinitialize(lgrData);
          lgr = NULL;
        }
      }

      if (!lgr)
      {
        // probe for a a suitable renderer
        for(const LG_Renderer **r = &LG_Renderers[0]; *r; ++r)
        {
          if (!IS_LG_RENDERER_VALID(*r))
          {
            DEBUG_ERROR("FIXME: Renderer %d is invalid, skpping", (int)(r - &LG_Renderers[0]));
            continue;
          }

          lgrData = NULL;
          if (!(*r)->initialize(&lgrData, lgrParams, lgrFormat))
          {
            (*r)->deinitialize(lgrData);
            continue;
          }

          lgr = *r;
          DEBUG_INFO("Initialized %s", (*r)->get_name());
          break;
        }

        if (!lgr)
        {
          DEBUG_INFO("Unable to find a suitable renderer");
          return -1;
        }
      }

      state.srcRect.x = 0;
      state.srcRect.y = 0;
      state.srcRect.w = header.width;
      state.srcRect.h = header.height;
      updatePositionInfo();
    }

    if (!lgr->render(
      lgrData,
      state.dstRect,
      (uint8_t *)state.shm + header.dataPos,
      params.useMipmap
    ))
    {
      DEBUG_ERROR("Failed to render the frame");
      break;
    }

    if (!params.showFPS)
    {
      SDL_RenderPresent(state.renderer);
      presentTime = microtime();
      continue;
    }

    // for now render the frame counter here, we really should
    // move this into the renderers though.
    if (fpsTime > 1000000)
    {
      SDL_Surface *textSurface = NULL;
      if (textTexture)
      {
        SDL_DestroyTexture(textTexture);
        textTexture = NULL;
      }

      char str[32];
      const float avgFPS = 1000.0f / (((float)fpsTime / frameCount) / 1000.0f);
      snprintf(str, sizeof(str), "FPS: %8.4f", avgFPS);
      SDL_Color color = {0xff, 0xff, 0xff};
      if (!(textSurface = TTF_RenderText_Blended(state.font, str, color)))
      {
        DEBUG_ERROR("Failed to render text");
        break;
      }

      textRect.x = 5;
      textRect.y = 5;
      textRect.w = textSurface->w;
      textRect.h = textSurface->h;

      textTexture = SDL_CreateTextureFromSurface(state.renderer, textSurface);
      SDL_SetTextureBlendMode(textTexture, SDL_BLENDMODE_BLEND);
      SDL_FreeSurface(textSurface);

      fpsTime    = 0;
      frameCount = 0;
    }

    if (textTexture)
    {
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

      glColor4f(0.0f, 0.0f, 1.0f, 0.5f);
      glBegin(GL_TRIANGLE_STRIP);
        glVertex2i(textRect.x             , textRect.y             );
        glVertex2i(textRect.x + textRect.w, textRect.y             );
        glVertex2i(textRect.x             , textRect.y + textRect.h);
        glVertex2i(textRect.x + textRect.w, textRect.y + textRect.h);
      glEnd();


      float tw, th;
      glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
      SDL_GL_BindTexture(textTexture, &tw, &th);
      glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0.0f, 0.0f); glVertex2i(textRect.x             , textRect.y             );
        glTexCoord2f(tw  , 0.0f); glVertex2i(textRect.x + textRect.w, textRect.y             );
        glTexCoord2f(0.0f, th  ); glVertex2i(textRect.x             , textRect.y + textRect.h);
        glTexCoord2f(tw  , th  ); glVertex2i(textRect.x + textRect.w, textRect.y + textRect.h);
      glEnd();
      glDisable(GL_BLEND);
      SDL_GL_UnbindTexture(textTexture);
    }

    SDL_RenderPresent(state.renderer);

    ++frameCount;
    uint64_t  t = microtime();
    fpsTime    += t - presentTime;
    presentTime = t;
  }

  if (lgr)
    lgr->deinitialize(lgrData);

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
          x -= state.shm->mouseX;
          y -= state.shm->mouseY;
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
  memset(&state, 0, sizeof(state));
  state.running = true;
  state.scaleX  = 1.0f;
  state.scaleY  = 1.0f;

  delay_calibrate();

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

    state.font = TTF_OpenFont("/usr/share/fonts/truetype/freefont/FreeMono.ttf", 16);
    if (!state.font)
    {
      DEBUG_ERROR("TTL_OpenFont Failed");
      return -1;
    }
  }

  state.window = SDL_CreateWindow(
    "Looking Glass (Client)",
    params.center ? SDL_WINDOWPOS_CENTERED : params.x,
    params.center ? SDL_WINDOWPOS_CENTERED : params.y,
    params.w,
    params.h,
    (
      SDL_WINDOW_SHOWN |
      (params.allowResize ? SDL_WINDOW_RESIZABLE  : 0) |
      (params.borderless  ? SDL_WINDOW_BORDERLESS : 0)
    )
  );

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

  state.renderer = SDL_CreateRenderer(state.window, -1,
    SDL_RENDERER_ACCELERATED |
    (params.vsync ? SDL_RENDERER_PRESENTVSYNC : 0)
  );

  if (!state.renderer)
  {
    DEBUG_ERROR("failed to create renderer");
    return -1;
  }

  SDL_GL_SetSwapInterval(params.vsync ? 1 : 0);
  if (params.useBufferStorage)
  {
    const GLubyte * extensions = glGetString(GL_EXTENSIONS);
    if (gluCheckExtension((const GLubyte *)"GL_ARB_buffer_storage", extensions))
    {
      DEBUG_INFO("Using GL_ARB_buffer_storage");
      state.hasBufferStorage = true;
    }
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

  if (state.renderer)
    SDL_DestroyRenderer(state.renderer);

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
    "  -g        Disable OpenGL 4.3 Buffer Storage (GL_ARB_buffer_storage)\n"
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
  while((c = getopt(argc, argv, "hf:sc:p:jMgmvkanrdx:y:w:b:l")) != -1)
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

      case 'g':
        params.useBufferStorage = false;
        break;

      case 'm':
        params.useMipmap = false;
        break;

      case 'v':
        params.vsync = false;
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