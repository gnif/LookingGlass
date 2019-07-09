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

#include "main.h"
#include "config.h"

#include <getopt.h>
#include <signal.h>
#include <SDL2/SDL_syswm.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "common/debug.h"
#include "common/crash.h"
#include "common/KVMFR.h"
#include "common/stringutils.h"
#include "utils.h"
#include "kb.h"
#include "ll.h"

// forwards
static int cursorThread(void * unused);
static int renderThread(void * unused);
static int frameThread (void * unused);

struct AppState  state;

// this structure is initialized in config.c
struct AppParams params = { 0 };

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

static int renderThread(void * unused)
{
  if (!state.lgr->render_startup(state.lgrData, state.window))
  {
    state.running = false;
    return 1;
  }

  // start the cursor thread after render startup to prevent a race condition
  SDL_Thread *t_cursor = NULL;
  if (!(t_cursor = SDL_CreateThread(cursorThread, "cursorThread", NULL)))
  {
    DEBUG_ERROR("cursor create thread failed");
    return 1;
  }

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

  state.running = false;
  SDL_WaitThread(t_cursor, NULL);
  return 0;
}

static int cursorThread(void * unused)
{
  KVMFRCursor         header;
  LG_RendererCursor   cursorType     = LG_CURSOR_COLOR;
  uint32_t            version        = 0;

  memset(&header, 0, sizeof(KVMFRCursor));

  while(state.running)
  {
    // poll until we have cursor data
    if(!(state.shm->cursor.flags & KVMFR_CURSOR_FLAG_UPDATE) &&
        !(state.shm->cursor.flags & KVMFR_CURSOR_FLAG_POS))
    {
      if (!state.running)
        return 0;
      usleep(params.cursorPollInterval);
      continue;
    }

    // if the cursor was moved
    bool moved = false;
    if (state.shm->cursor.flags & KVMFR_CURSOR_FLAG_POS)
    {
      state.cursor.x      = state.shm->cursor.x;
      state.cursor.y      = state.shm->cursor.y;
      state.haveCursorPos = true;
      moved               = true;
    }

    // if this was only a move event
    if (!(state.shm->cursor.flags & KVMFR_CURSOR_FLAG_UPDATE))
    {
      // turn off the pos flag, trigger the event and continue
      __sync_and_and_fetch(&state.shm->cursor.flags, ~KVMFR_CURSOR_FLAG_POS);

      state.lgr->on_mouse_event
      (
        state.lgrData,
        state.cursorVisible,
        state.cursor.x,
        state.cursor.y
      );
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
    if (showCursor != state.cursorVisible || moved)
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

static int frameThread(void * unused)
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

      usleep(params.framePollInterval);
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
      DEBUG_WARN("  width  : %u"     , header.width  );
      DEBUG_WARN("  height : %u"     , header.height );
      DEBUG_WARN("  pitch  : %u"     , header.pitch  );
      DEBUG_WARN("  dataPos: 0x%08lx", header.dataPos);
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

static LG_ClipboardData spice_type_to_clipboard_type(const SpiceDataType type)
{
  switch(type)
  {
    case SPICE_DATA_TEXT: return LG_CLIPBOARD_DATA_TEXT; break;
    case SPICE_DATA_PNG : return LG_CLIPBOARD_DATA_PNG ; break;
    case SPICE_DATA_BMP : return LG_CLIPBOARD_DATA_BMP ; break;
    case SPICE_DATA_TIFF: return LG_CLIPBOARD_DATA_TIFF; break;
    case SPICE_DATA_JPEG: return LG_CLIPBOARD_DATA_JPEG; break;
    default:
      DEBUG_ERROR("invalid spice data type");
      return LG_CLIPBOARD_DATA_NONE;
  }
}

static SpiceDataType clipboard_type_to_spice_type(const LG_ClipboardData type)
{
  switch(type)
  {
    case LG_CLIPBOARD_DATA_TEXT: return SPICE_DATA_TEXT; break;
    case LG_CLIPBOARD_DATA_PNG : return SPICE_DATA_PNG ; break;
    case LG_CLIPBOARD_DATA_BMP : return SPICE_DATA_BMP ; break;
    case LG_CLIPBOARD_DATA_TIFF: return SPICE_DATA_TIFF; break;
    case LG_CLIPBOARD_DATA_JPEG: return SPICE_DATA_JPEG; break;
    default:
      DEBUG_ERROR("invalid clipboard data type");
      return SPICE_DATA_NONE;
  }
}

void clipboardRelease()
{
  if (!params.clipboardToVM)
    return;

  spice_clipboard_release();
}

void clipboardNotify(const LG_ClipboardData type)
{
  if (!params.clipboardToVM)
    return;

  if (type == LG_CLIPBOARD_DATA_NONE)
  {
    spice_clipboard_release();
    return;
  }

  spice_clipboard_grab(clipboard_type_to_spice_type(type));
}

void clipboardData(const LG_ClipboardData type, uint8_t * data, size_t size)
{
  if (!params.clipboardToVM)
    return;

  uint8_t * buffer = data;

  // unix2dos
  if (type == LG_CLIPBOARD_DATA_TEXT)
  {
    // TODO: make this more memory efficent
    size_t newSize = 0;
    buffer = malloc(size * 2);
    uint8_t * p = buffer;
    for(uint32_t i = 0; i < size; ++i)
    {
      uint8_t c = data[i];
      if (c == '\n')
      {
        *p++ = '\r';
        ++newSize;
      }
      *p++ = c;
      ++newSize;
    }
    size = newSize;
  }

  spice_clipboard_data(clipboard_type_to_spice_type(type), buffer, (uint32_t)size);
  if (buffer != data)
    free(buffer);
}

void clipboardRequest(const LG_ClipboardReplyFn replyFn, void * opaque)
{
  if (!params.clipboardToLocal)
    return;

  struct CBRequest * cbr = (struct CBRequest *)malloc(sizeof(struct CBRequest()));

  cbr->type    = state.cbType;
  cbr->replyFn = replyFn;
  cbr->opaque  = opaque;
  ll_push(state.cbRequestList, cbr);

  spice_clipboard_request(state.cbType);
}

void spiceClipboardNotice(const SpiceDataType type)
{
  if (!params.clipboardToLocal)
    return;

  if (!state.lgc || !state.lgc->notice)
    return;

  state.cbType = type;
  state.lgc->notice(clipboardRequest, spice_type_to_clipboard_type(type));
}

void spiceClipboardData(const SpiceDataType type, uint8_t * buffer, uint32_t size)
{
  if (!params.clipboardToLocal)
    return;

  if (type == SPICE_DATA_TEXT)
  {
    // dos2unix
    uint8_t  * p       = buffer;
    uint32_t   newSize = size;
    for(uint32_t i = 0; i < size; ++i)
    {
      uint8_t c = buffer[i];
      if (c == '\r')
      {
        --newSize;
        continue;
      }
      *p++ = c;
    }
    size = newSize;
  }

  struct CBRequest * cbr;
  if (ll_shift(state.cbRequestList, (void **)&cbr))
  {
    cbr->replyFn(cbr->opaque, type, buffer, size);
    free(cbr);
  }
}

void spiceClipboardRelease()
{
  if (!params.clipboardToLocal)
    return;

  if (state.lgc && state.lgc->release)
    state.lgc->release();
}

void spiceClipboardRequest(const SpiceDataType type)
{
  if (!params.clipboardToVM)
    return;

  if (state.lgc && state.lgc->request)
    state.lgc->request(spice_type_to_clipboard_type(type));
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

        // allow a window close event to close the application even if ignoreQuit is set
        case SDL_WINDOWEVENT_CLOSE:
          state.running = false;
          break;
      }
      return 0;
    }

    case SDL_SYSWMEVENT:
    {
      if (params.useSpiceClipboard && state.lgc && state.lgc->wmevent)
        state.lgc->wmevent(event->syswm.msg);
      return 0;
    }

    case SDL_MOUSEMOTION:
    {
      if (state.ignoreInput || !params.useSpiceInput)
        break;

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
        state.accX  = 0;
        state.accY  = 0;
        state.sensX = 0;
        state.sensY = 0;

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

        if (serverMode && state.mouseSens != 0)
        {
          state.sensX += ((float)x / 10.0f) * (state.mouseSens + 10);
          state.sensY += ((float)y / 10.0f) * (state.mouseSens + 10);
          x = floor(state.sensX);
          y = floor(state.sensY);
          state.sensX -= x;
          state.sensY -= y;
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
      if (sc == params.escapeKey)
      {
        state.escapeActive = true;
        state.escapeAction = -1;
        break;
      }

      if (state.escapeActive)
      {
        state.escapeAction = sc;
        break;
      }

      if (state.ignoreInput || !params.useSpiceInput)
        break;

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
      if (state.escapeActive)
      {
        if (state.escapeAction == -1)
        {
          if (params.useSpiceInput)
          {
            serverMode = !serverMode;
            spice_mouse_mode(serverMode);
            SDL_SetRelativeMouseMode(serverMode);
            SDL_SetWindowGrab(state.window, serverMode);
            DEBUG_INFO("Server Mode: %s", serverMode ? "on" : "off");

            app_alert(
              serverMode ? LG_ALERT_SUCCESS  : LG_ALERT_WARNING,
              serverMode ? "Capture Enabled" : "Capture Disabled"
            );

            if (!serverMode)
              realignGuest = true;
          }
        }
        else
        {
          KeybindHandle handle = state.bindings[sc];
          if (handle)
            handle->callback(sc, handle->opaque);
        }

        if (sc == params.escapeKey)
          state.escapeActive = false;
      }

      if (state.ignoreInput || !params.useSpiceInput)
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
      if (state.ignoreInput || !params.useSpiceInput)
        break;

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
      if (state.ignoreInput || !params.useSpiceInput)
        break;

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
      if (state.ignoreInput || !params.useSpiceInput)
        break;

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

void int_handler(int signal)
{
  switch(signal)
  {
    case SIGINT:
    case SIGTERM:
      DEBUG_INFO("Caught signal, shutting down...");
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
  const LG_Renderer *r = LG_Renderers[index];

  if (!IS_LG_RENDERER_VALID(r))
  {
    DEBUG_ERROR("FIXME: Renderer %d is invalid, skipping", index);
    return false;
  }

  // create the renderer
  state.lgrData = NULL;
  if (!r->create(&state.lgrData, lgrParams))
    return false;

  // initialize the renderer
  if (!r->initialize(state.lgrData, sdlFlags))
  {
    r->deinitialize(state.lgrData);
    return false;
  }

  DEBUG_INFO("Using Renderer: %s", r->get_name());
  return true;
}

static void toggle_fullscreen(SDL_Scancode key, void * opaque)
{
  SDL_SetWindowFullscreen(state.window, params.fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
  params.fullscreen = !params.fullscreen;
}

static void toggle_input(SDL_Scancode key, void * opaque)
{
  state.ignoreInput = !state.ignoreInput;
  app_alert(
    LG_ALERT_INFO,
    state.ignoreInput ? "Input Disabled" : "Input Enabled"
  );
}

static void mouse_sens_inc(SDL_Scancode key, void * opaque)
{
  char * msg;
  if (state.mouseSens < 9)
    ++state.mouseSens;

  alloc_sprintf(&msg, "Sensitivity: %s%d", state.mouseSens > 0 ? "+" : "", state.mouseSens);
  app_alert(
    LG_ALERT_INFO,
    msg
  );
  free(msg);
}

static void mouse_sens_dec(SDL_Scancode key, void * opaque)
{
  char * msg;

  if (state.mouseSens > -9)
    --state.mouseSens;

  alloc_sprintf(&msg, "Sensitivity: %s%d", state.mouseSens > 0 ? "+" : "", state.mouseSens);
  app_alert(
    LG_ALERT_INFO,
    msg
  );
  free(msg);
}

static void ctrl_alt_fn(SDL_Scancode key, void * opaque)
{
  const uint32_t ctrl = mapScancode(SDL_SCANCODE_LCTRL);
  const uint32_t alt  = mapScancode(SDL_SCANCODE_LALT );
  const uint32_t fn   = mapScancode(key);

  spice_key_down(ctrl);
  spice_key_down(alt );
  spice_key_down(fn  );

  spice_key_up(ctrl);
  spice_key_up(alt );
  spice_key_up(fn  );
}

static void register_key_binds()
{
  state.kbFS           = app_register_keybind(SDL_SCANCODE_F     , toggle_fullscreen, NULL);
  state.kbInput        = app_register_keybind(SDL_SCANCODE_I     , toggle_input     , NULL);
  state.kbMouseSensInc = app_register_keybind(SDL_SCANCODE_INSERT, mouse_sens_inc   , NULL);
  state.kbMouseSensDec = app_register_keybind(SDL_SCANCODE_DELETE, mouse_sens_dec   , NULL);

  state.kbCtrlAltFn[0 ] = app_register_keybind(SDL_SCANCODE_F1 , ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[1 ] = app_register_keybind(SDL_SCANCODE_F2 , ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[2 ] = app_register_keybind(SDL_SCANCODE_F3 , ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[3 ] = app_register_keybind(SDL_SCANCODE_F4 , ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[4 ] = app_register_keybind(SDL_SCANCODE_F5 , ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[5 ] = app_register_keybind(SDL_SCANCODE_F6 , ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[6 ] = app_register_keybind(SDL_SCANCODE_F7 , ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[7 ] = app_register_keybind(SDL_SCANCODE_F8 , ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[8 ] = app_register_keybind(SDL_SCANCODE_F9 , ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[9 ] = app_register_keybind(SDL_SCANCODE_F10, ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[10] = app_register_keybind(SDL_SCANCODE_F11, ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[11] = app_register_keybind(SDL_SCANCODE_F12, ctrl_alt_fn, NULL);
}

static void release_key_binds()
{
  app_release_keybind(&state.kbFS);
  app_release_keybind(&state.kbInput);
  for(int i = 0; i < 12; ++i)
    app_release_keybind(&state.kbCtrlAltFn[i]);
}

int run()
{
  DEBUG_INFO("Looking Glass (" BUILD_VERSION ")");
  DEBUG_INFO("Locking Method: " LG_LOCK_MODE);

  memset(&state, 0, sizeof(state));
  state.running   = true;
  state.scaleX    = 1.0f;
  state.scaleY    = 1.0f;

  state.mouseSens = params.mouseSens;
       if (state.mouseSens < -9) state.mouseSens = -9;
  else if (state.mouseSens >  9) state.mouseSens =  9;

  char* XDG_SESSION_TYPE = getenv("XDG_SESSION_TYPE");

  if (XDG_SESSION_TYPE == NULL)
    XDG_SESSION_TYPE = "unspecified";

  if (strcmp(XDG_SESSION_TYPE, "wayland") == 0)
  {
     DEBUG_INFO("Wayland detected");
     if (getenv("SDL_VIDEODRIVER") == NULL)
     {
       int err = setenv("SDL_VIDEODRIVER", "wayland", 1);
       if (err < 0)
       {
         DEBUG_ERROR("Unable to set the env variable SDL_VIDEODRIVER: %d", err);
         return -1;
       }
       DEBUG_INFO("SDL_VIDEODRIVER has been set to wayland");
     }
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
  signal(SIGINT , int_handler);
  signal(SIGTERM, int_handler);

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
    params.windowTitle,
    params.center ? SDL_WINDOWPOS_CENTERED : params.x,
    params.center ? SDL_WINDOWPOS_CENTERED : params.y,
    params.w,
    params.h,
    (
      SDL_WINDOW_SHOWN |
      (params.fullscreen  ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) |
      (params.allowResize ? SDL_WINDOW_RESIZABLE  : 0) |
      (params.borderless  ? SDL_WINDOW_BORDERLESS : 0) |
      (params.maximize    ? SDL_WINDOW_MAXIMIZED  : 0) |
      sdlFlags
    )
  );

  if (state.window == NULL)
  {
    DEBUG_ERROR("Could not create an SDL window: %s\n", SDL_GetError());
    return 1;
  }

  if (params.fullscreen || !params.minimizeOnFocusLoss)
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

  if (!params.noScreensaver)
  {
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    SDL_EnableScreenSaver();
  }

  if (!params.center)
    SDL_SetWindowPosition(state.window, params.x, params.y);

  // ensure the initial window size is stored in the state
  SDL_GetWindowSize(state.window, &state.windowW, &state.windowH);

  // ensure renderer viewport is aware of the current window size
  updatePositionInfo();

  //Auto detect active monitor refresh rate for FPS Limit if no FPS Limit was passed.
  if (params.fpsLimit == -1)
  {
      SDL_DisplayMode current;
      if (SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(state.window), &current) == 0)
      {
          state.frameTime = 1e9 / (current.refresh_rate * 2);
      }
      else 
      {
          DEBUG_WARN("Unable to capture monitor refresh rate using the default FPS Limit: 200");
          state.frameTime = 1e9 / 200;
      }
  }
  else 
  {
      DEBUG_INFO("Using the FPS Limit from args: %d", params.fpsLimit);
      state.frameTime = 1e9 / params.fpsLimit;
  }
  
  register_key_binds();

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

      state.lgc = LG_Clipboards[0];
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

  if (state.lgc)
  {
    DEBUG_INFO("Using Clipboard: %s", state.lgc->getName());
    if (!state.lgc->init(&wminfo, clipboardRelease, clipboardNotify, clipboardData))
    {
      DEBUG_WARN("Failed to initialize the clipboard interface, continuing anyway");
      state.lgc = NULL;
    }

    state.cbRequestList = ll_new();
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

    if (params.useSpiceInput || params.useSpiceClipboard)
    {
      spice_set_clipboard_cb(
          spiceClipboardNotice,
          spiceClipboardData,
          spiceClipboardRelease,
          spiceClipboardRequest);

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
          if (state.lgr && params.showAlerts)
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

  // if spice is still connected send key up events for any pressed keys
  if (params.useSpiceInput && spice_ready())
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

  if (state.lgc)
  {
    state.lgc->free();

    struct CBRequest *cbr;
    while(ll_shift(state.cbRequestList, (void **)&cbr))
      free(cbr);
    ll_free(state.cbRequestList);
  }

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

int main(int argc, char * argv[])
{
  if (!installCrashHandler("/proc/self/exe"))
    DEBUG_WARN("Failed to install the crash handler");

  config_init();

  // early renderer setup for option registration
  for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
    LG_Renderers[i]->setup();

  if (!config_load(argc, argv))
    return -1;

  if (params.grabKeyboard)
    SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");

  const int ret = run();
  release_key_binds();

  config_free();
  return ret;
}
