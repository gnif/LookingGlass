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
#include <SDL_ttf.h>
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

  TTF_Font  *font;
  SDL_Rect  srcRect, dstRect;
  float     scaleX, scaleY;

  SDL_Window          * window;
  SDL_Renderer        * renderer;
  struct KVMGFXHeader * shm;
  unsigned int          shmSize;
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
  struct KVMGFXHeader header;
  struct KVMGFXHeader newHeader;
  SDL_Texture        *texture      = NULL;
  GLuint              vboID[2]     = {0, 0};
  GLuint              intFormat    = 0;
  GLuint              vboFormat    = 0;
  GLuint              vboTex[2]    = {0, 0};
  unsigned int        texIndex     = 0;
  unsigned int        texSize      = 0;
  uint8_t            *pixels       = (uint8_t*)state.shm;
  uint8_t            *texPixels[2] = {NULL, NULL};

  unsigned int        ticks        = SDL_GetTicks();
  unsigned int        frameCount   = 0;
  SDL_Texture        *textTexture  = NULL;
  SDL_Rect            textRect     = {0, 0, 0, 0};

  memset(&header, 0, sizeof(struct KVMGFXHeader));

  // kick the guest early for our intial frame
  // the guestID may be invalid, it doesn't matter
  ivshmem_kick_irq(state.shm->guestID, 0);

  while(state.running)
  {
    // copy the header for our use
    memcpy(&newHeader, state.shm, sizeof(struct KVMGFXHeader));
    ivshmem_kick_irq(newHeader.guestID, 0);

    // ensure the header magic is valid, this will help prevent crash out when the memory hasn't yet been initialized
    if (
      memcmp(newHeader.magic, KVMGFX_HEADER_MAGIC, sizeof(KVMGFX_HEADER_MAGIC)) != 0 ||
      newHeader.version != KVMGFX_HEADER_VERSION
    )
    {
      usleep(1000);
      continue;
    }

    // if the header is invalid or it has changed
    if (!areFormatsSame(header, newHeader))
    {
      if (state.hasBufferStorage)
      {
        if (vboID[0])
        {
          if (vboTex[0])
          {
            glDeleteTextures(1, vboTex);
            memset(vboTex, 0, sizeof(vboTex));
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
      switch(newHeader.frameType)
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
          header.frameType = FRAME_TYPE_INVALID;
          continue;
      }

      // update the window size and create the render texture
      if (params.autoResize)
      {
        SDL_SetWindowSize(state.window, newHeader.width, newHeader.height);
        if (params.center)
          SDL_SetWindowPosition(state.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
      }

      if (state.hasBufferStorage)
      {
        // calculate the texture size in bytes
        texSize = newHeader.width * newHeader.stride * bpp;

        // ensure the size makes sense
        if (newHeader.dataPos + texSize > state.shmSize)
        {
          DEBUG_ERROR("The guest sent an invalid dataPos");
          break;
        }

        // setup two buffers so we don't have to use fences
        glGenBuffers(2, vboID);
        for (int i = 0; i < 2; ++i)
        {
          glBindBuffer(GL_PIXEL_UNPACK_BUFFER, vboID[i]);
          glBufferStorage(GL_PIXEL_UNPACK_BUFFER, texSize, 0, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT);
          texPixels[i] = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, texSize,
              GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
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
        glGenTextures(1, vboTex);
        glBindTexture(GL_TEXTURE_2D, vboTex[0]);
        glTexImage2D(
          GL_TEXTURE_2D,
          0,
          intFormat,
          newHeader.width, newHeader.height,
          0,
          vboFormat,
          GL_UNSIGNED_BYTE,
          (void*)0
        );
        glBindTexture(GL_TEXTURE_2D, 0);
      }
      else
      {
        texture = SDL_CreateTexture(state.renderer, sdlFormat, SDL_TEXTUREACCESS_STREAMING, newHeader.width, newHeader.height);
        if (!texture)
        {
          DEBUG_ERROR("Failed to create a texture");
          break;
        }
      }

      ticks = SDL_GetTicks();
      frameCount = 0;

      memcpy(&header, &newHeader, sizeof(header));
      state.srcRect.x = 0;
      state.srcRect.y = 0;
      state.srcRect.w = header.width;
      state.srcRect.h = header.height;
      updatePositionInfo();
    }

    //beyond this point DO NOT use state.shm for security

    // final sanity checks on the data presented by the guest
    // this is critical as the guest could overflow this buffer to
    // try to take control of the host
    if (newHeader.dataPos + texSize > state.shmSize)
    {
      DEBUG_ERROR("The guest sent an invalid dataPos");
      break;
    }

    SDL_RenderClear(state.renderer);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

    if (state.hasBufferStorage)
    {
      // copy the buffer to the texture and let the guest advance
      memcpySSE(texPixels[texIndex], pixels + newHeader.dataPos, texSize);
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, vboID[texIndex]);
      glFlushMappedBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, texSize);

      // bind the texture and update it
      glBindTexture(GL_TEXTURE_2D       , vboTex[0]   );
      glPixelStorei(GL_UNPACK_ALIGNMENT , 1           );
      glPixelStorei(GL_UNPACK_ROW_LENGTH, header.width);
      glTexSubImage2D(
          GL_TEXTURE_2D,
          0,
          0, 0,
          header.width, header.height,
          vboFormat,
          GL_UNSIGNED_BYTE,
          (void*)0
      );
      if (params.useMipmap)
        glGenerateMipmap(GL_TEXTURE_2D);

      // unbind the buffer
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

      // configure the texture
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      if (params.useMipmap)
      {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
      }
      else
      {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      }

      // draw the screen
      glEnable(GL_TEXTURE_2D);
      glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0.0f, 0.0f); glVertex2i(state.dstRect.x                  , state.dstRect.y                  );
        glTexCoord2f(1.0f, 0.0f); glVertex2i(state.dstRect.x + state.dstRect.w, state.dstRect.y                  );
        glTexCoord2f(0.0f, 1.0f); glVertex2i(state.dstRect.x                  , state.dstRect.y + state.dstRect.h);
        glTexCoord2f(1.0f, 1.0f); glVertex2i(state.dstRect.x + state.dstRect.w, state.dstRect.y + state.dstRect.h);
      glEnd();
      glDisable(GL_TEXTURE_2D);
      glBindTexture(GL_TEXTURE_2D, 0);

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
      texSize = header.height * pitch;

      // copy the buffer to the texture and let the guest advance
      memcpySSE(texPixels[texIndex], pixels + newHeader.dataPos, texSize);

      SDL_UnlockTexture(texture);
      SDL_RenderCopy(state.renderer, texture, NULL, &state.dstRect);
    }

    if (params.showFPS)
    {
      if (++frameCount == 10)
      {
        SDL_Surface *textSurface = NULL;
        if (textTexture)
        {
          SDL_DestroyTexture(textTexture);
          textTexture = NULL;
        }
        const unsigned int time = SDL_GetTicks();
        const float avgFPS = (float)frameCount / ((time - ticks) / 1000.0f);
        char strFPS[12];
        snprintf(strFPS, sizeof(strFPS), "FPS: %6.2f", avgFPS);
        SDL_Color color = {0xff, 0xff, 0xff};
        if (!(textSurface = TTF_RenderText_Blended(state.font, strFPS, color)))
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

        frameCount = 0;
        ticks = time;
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
    }

    SDL_RenderPresent(state.renderer);
    state.started = true;

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
          ivshmem_kick_irq(newHeader.guestID, 0);
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
  }

  state.running = false;
  if (state.hasBufferStorage)
  {
    glDeleteTextures(1, vboTex        );
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
  bool keyDown[SDL_NUM_SCANCODES];

  memset(keyDown, 0, sizeof(keyDown));

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
          event.motion.x < state.dstRect.x                   ||
          event.motion.x > state.dstRect.x + state.dstRect.w ||
          event.motion.y < state.dstRect.y                   ||
          event.motion.y > state.dstRect.y + state.dstRect.h
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
    "KVM-GFX Test",
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

  if (params.useBufferStorage)
  {
    const GLubyte * extensions = glGetString(GL_EXTENSIONS);
    if (gluCheckExtension((const GLubyte *)"GL_ARB_buffer_storage", extensions))
    {
      DEBUG_INFO("Using GL_ARB_buffer_storage");
      state.hasBufferStorage = true;
    }
  }

  if (params.vsync)
  {
    // try for late swap tearing to help keep sync with the guest
    if (SDL_GL_SetSwapInterval(-1) == -1)
      SDL_GL_SetSwapInterval(1);
  }
  else
    SDL_GL_SetSwapInterval(0);

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