/*
KVMGFX Client - A KVM Client for VGA Passthrough
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
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
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
#include "memcpySSE.h"
#include "KVMGFXHeader.h"
#include "ivshmem/ivshmem.h"
#include "spice/spice.h"
#include "kb.h"

struct AppState
{
  bool      hasBufferStorage;

  bool      running;
  bool      started;
  bool      windowChanged;

  SDL_Window          * window;
  SDL_Renderer        * renderer;
  struct KVMGFXHeader * shm;
};

struct AppParams
{
  bool         borderless;
  const char * ivshmemSocket;
  bool         useBufferStorage;
  bool         useSpice;
  const char * spiceHost;
  unsigned int spicePort;
};

struct AppState  state;
struct AppParams params =
{
  .borderless       = true,
  .ivshmemSocket    = "/tmp/ivshmem_socket",
  .useBufferStorage = true,
  .useSpice         = true,
  .spiceHost        = "127.0.0.1",
  .spicePort        = 5900
};

inline bool areFormatsSame(const struct KVMGFXHeader s1, const struct KVMGFXHeader s2)
{
  return
    (s1.frameType != FRAME_TYPE_INVALID) &&
    (s2.frameType != FRAME_TYPE_INVALID) &&
    (s1.version   == s2.version  ) &&
    (s1.frameType == s2.frameType) &&
    (s1.width     == s2.width    ) &&
    (s1.height    == s2.height   );
}

int renderThread(void * unused)
{
  struct KVMGFXHeader format;
  SDL_Texture        *texture      = NULL;
  GLuint              vboID[2]     = {0, 0};
  GLuint              intFormat    = 0;
  GLuint              vboFormat    = 0;
  GLuint              vboTex       = 0;
  unsigned int        texIndex     = 0;
  unsigned int        texSize      = 0;
  uint8_t            *pixels       = (uint8_t*)state.shm;
  uint8_t            *texPixels[2] = {NULL, NULL};

  format.version   = 1;
  format.frameType = FRAME_TYPE_INVALID;
  format.width     = 0;
  format.height    = 0;
  format.stride    = 0;

  // kick the guest early for our intial frame
  // the guestID may be invalid, it doesn't matter
  ivshmem_kick_irq(state.shm->guestID, 0);

  while(state.running)
  {
    // ensure the header magic is valid, this will help prevent crash out when the memory hasn't yet been initialized
    if (memcmp(state.shm->magic, KVMGFX_HEADER_MAGIC, sizeof(KVMGFX_HEADER_MAGIC)) != 0)
      continue;

    if (state.shm->version != KVMGFX_HEADER_VERSION)
      continue;

    bool ready = false;
    bool error = false;
    while(state.running && !ready && !error)
    {
      // kick the guest and wait for a frame
      switch(ivshmem_wait_irq(0))
      {
        case IVSHMEM_WAIT_RESULT_OK:
          ready = true;
          break;

        case IVSHMEM_WAIT_RESULT_TIMEOUT:
          ivshmem_kick_irq(state.shm->guestID, 0);
          ready = false;
          break;

        case IVSHMEM_WAIT_RESULT_ERROR:
          error = true;
          break;
      }
    }

    if (error)
    {
      DEBUG_ERROR("error during wait for host");
      break;
    }

    // if the format is invalid or it has changed
    if (!areFormatsSame(format, *state.shm))
    {
      if (state.hasBufferStorage)
      {
        if (vboID[0])
        {
          if (vboTex)
          {
            glDeleteTextures(1, &vboTex);
            vboTex = 0;
          }

          glUnmapBuffer(GL_TEXTURE_BUFFER);
          glDeleteBuffers(2, vboID);
          memset(vboID, 0, sizeof(vboID));
        }
      }
      else
      {
        if (texture)
        {
          SDL_DestroyTexture(texture);
          texture = NULL;
        }
      }

      Uint32  sdlFormat;
      unsigned int bpp;
      switch(state.shm->frameType)
      {
        case FRAME_TYPE_ARGB:
          sdlFormat = SDL_PIXELFORMAT_ARGB8888;
          bpp       = 4;
          intFormat = GL_RGBA8;
          vboFormat = GL_BGRA;
          break;

        case FRAME_TYPE_RGB:
          sdlFormat = SDL_PIXELFORMAT_RGB24;
          bpp       = 3;
          intFormat = GL_RGB8;
          vboFormat = GL_BGR;
          break;

        default:
          format.frameType = FRAME_TYPE_INVALID;
          continue;
      }

      // update the window size and create the render texture
      SDL_SetWindowSize(state.window, state.shm->width, state.shm->height);
      SDL_SetWindowPosition(state.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

      if (state.hasBufferStorage)
      {
        // calculate the texture size in bytes
        texSize = state.shm->width * state.shm->stride * bpp;

        // setup two buffers so we don't have to use fences
        glGenBuffers(2, vboID);
        for (int i = 0; i < 2; ++i)
        {
          glBindBuffer(GL_PIXEL_UNPACK_BUFFER, vboID[i]);
                         glBufferStorage (GL_PIXEL_UNPACK_BUFFER, texSize, 0, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
          texPixels[i] = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, texSize, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
          if (!texPixels[i])
          {
            DEBUG_ERROR("Failed to map buffer range, turning off buffer storage");
            state.hasBufferStorage = false;
            break;
          }
          glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        }

        if (!state.hasBufferStorage)
        {
          texIndex = 0;
          glDeleteBuffers(2, vboID);
          memset(vboID, 0, sizeof(vboID));
          continue;
        }

        // create the texture
        glGenTextures(1, &vboTex);
        glBindTexture(GL_TEXTURE_2D, vboTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S    , GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T    , GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexImage2D(
          GL_TEXTURE_2D,
          0,
          intFormat,
          state.shm->width, state.shm->height,
          0,
          vboFormat,
          GL_UNSIGNED_BYTE,
          (void*)0
        );
        glBindTexture(GL_TEXTURE_2D, 0);
      }
      else
      {
        texture = SDL_CreateTexture(state.renderer, sdlFormat, SDL_TEXTUREACCESS_STREAMING, state.shm->width, state.shm->height);
        if (!texture)
        {
          DEBUG_ERROR("Failed to create a texture");
          break;
        }
      }

      memcpy(&format, state.shm, sizeof(format));
      state.windowChanged = true;
    }

    if (state.hasBufferStorage)
    {
      // copy the buffer to the texture and let the guest advance
      memcpySSE(texPixels[texIndex], pixels + state.shm->dataPos, texSize);
      ivshmem_kick_irq(state.shm->guestID, 0);

      // update the texture
      glEnable(GL_TEXTURE_2D);
      glBindTexture(GL_TEXTURE_2D, vboTex);
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, vboID[texIndex ? 0 : 1]);
      glTexSubImage2D(
          GL_TEXTURE_2D,
          0,
          0, 0,
          state.shm->width, state.shm->height,
          vboFormat,
          GL_UNSIGNED_BYTE,
          (void*)0
      );
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

      // draw the screen
      glBegin(GL_TRIANGLE_STRIP);
      glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f            , 0.0f             );
      glTexCoord2f(1.0f, 0.0f); glVertex2f(state.shm->width, 0.0f             );
      glTexCoord2f(0.0f, 1.0f); glVertex2f(0.0f            , state.shm->height);
      glTexCoord2f(1.0f, 1.0f); glVertex2f(state.shm->width, state.shm->height);
      glEnd();
      glBindTexture(GL_TEXTURE_2D, 0);
      glDisable(GL_TEXTURE_2D);

      // update our texture index
      if (++texIndex == 2)
        texIndex = 0;
    }
    else
    {
      int pitch;
      if (SDL_LockTexture(texture, NULL, (void**)&texPixels[0], &pitch) != 0)
      {
        DEBUG_ERROR("Failed to lock the texture for update");
        break;
      }
      texSize = state.shm->height * pitch;

      // copy the buffer to the texture and let the guest advance
      memcpySSE(texPixels[texIndex], pixels + state.shm->dataPos, texSize);
      ivshmem_kick_irq(state.shm->guestID, 0);

      SDL_UnlockTexture(texture);
      SDL_RenderCopy(state.renderer, texture, NULL, NULL);
    }

    SDL_RenderPresent(state.renderer);

    state.started = true;
  }

  state.running = false;
  if (state.hasBufferStorage)
  {
    glDeleteTextures(1, &vboTex       );
    glUnmapBuffer   (GL_TEXTURE_BUFFER);
    glDeleteBuffers (2, vboID         );
  }
  else
    if (texture)
      SDL_DestroyTexture(texture);

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

  // ensure mouse acceleration is identical in server mode
  SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "1", SDL_HINT_OVERRIDE);

  while(state.running)
  {
    SDL_Event event;
    if (!SDL_WaitEventTimeout(&event, 1000))
    {
      const char * err = SDL_GetError();
      if (err[0] != '\0')
      {
        DEBUG_ERROR("SDL Error: %s", err);
        state.running = false;
        break;
      }
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

        if (!spice_key_down(scancode))
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


        uint32_t scancode = mapScancode(sc);
        if (scancode == 0)
          break;

        if (!spice_key_up(scancode))
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
        int x = 0;
        int y = 0;
        if (realignGuest || state.windowChanged)
        {
          x = event.motion.x - state.shm->mouseX;
          y = event.motion.y - state.shm->mouseY;
          realignGuest        = false;
          state.windowChanged = false;
        }
        else
        {
          x = event.motion.xrel;
          y = event.motion.yrel;
        }

        if (x != 0 || y != 0)
          if (!spice_mouse_motion(x, y))
          {
            DEBUG_ERROR("SDL_MOUSEMOTION: failed to send message");
            break;
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

  if (SDL_Init(SDL_INIT_VIDEO) < 0)
  {
    DEBUG_ERROR("SDL_Init Failed");
    return -1;
  }

  state.window = SDL_CreateWindow(
      "KVM-GFX Test",
      SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED,
      100, 100,
      params.borderless ? SDL_WINDOW_BORDERLESS : 0
  );

  if (!state.window)
  {
    DEBUG_ERROR("failed to create window");
    return -1;
  }

  // work around SDL_ShowCursor being non functional
  SDL_Cursor *cursor = NULL;
  int32_t cursorData[2] = {0, 0};
  cursor = SDL_CreateCursor((uint8_t*)cursorData, (uint8_t*)cursorData, 8, 8, 4, 4);
  SDL_SetCursor(cursor);
  SDL_ShowCursor(SDL_DISABLE);

  state.renderer = SDL_CreateRenderer(state.window, -1,
      SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

  if (params.useBufferStorage)
  {
    const GLubyte * extensions = glGetString(GL_EXTENSIONS);
    if (gluCheckExtension((const GLubyte *)"GL_ARB_buffer_storage", extensions))
    {
      DEBUG_INFO("Using GL_ARB_buffer_storage");
      state.hasBufferStorage = true;
    }
  }

  if (!state.renderer)
  {
    DEBUG_ERROR("failed to create window");
    return -1;
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

    state.shm = (struct KVMGFXHeader *)ivshmem_get_map();
    if (!state.shm)
    {
      DEBUG_ERROR("Failed to map memory");
      break;
    }
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

  SDL_Quit();
  return 0;
}

void doHelp(char * app)
{
  fprintf(stderr,
    "Usage: %s [OPTION]...\n"
    "Example: %s -h\n"
    "\n"
    "  -h       Print out this help\n"
    "  -f PATH  Specify the path to the ivshmem socket [current: %s]\n"
    "  -d       Disable OpenGL 4.3 Buffer Storage (GL_ARB_buffer_storage)\n"
    "  -s       Disable spice client\n"
    "  -c HOST  Specify the spice host [current: %s]\n"
    "  -p PORT  Specify the spice port [current: %d]\n"
    "  -l       License information\n"
    "\n",
    app,
    app,
    params.ivshmemSocket,
    params.spiceHost,
    params.spicePort
  );
}

void doLicense()
{
  fprintf(stderr,
    "\n"
    "KVMGFX Client - A KVM Client for VGA Passthrough\n"
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
  while((c = getopt(argc, argv, "hf:dsc:p:l")) != -1)
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

      case 'd':
        params.useBufferStorage = false;
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

      case 'l':
        doLicense();
        return 0;

    }

  return run();
}