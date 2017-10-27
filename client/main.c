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

#include <GL/gl.h>

#define DEBUG
#include "debug.h"
#include "KVMGFXHeader.h"
#include "ivshmem/ivshmem.h"
#include "spice/spice.h"
#include "kb.h"

#define MAP_SIZE (16*1024*1024)
#define COPY_THREADS 4

typedef void (*CompFunc)(uint8_t * dst, const uint8_t * src, const size_t len);
typedef void (*DrawFunc)(CompFunc compFunc, SDL_Texture * texture, uint8_t * dst, const uint8_t * src);

typedef struct
{
  SDL_mutex  * mutex;
  SDL_cond   * cond;
  SDL_Thread * thread;

  bool  rdy;
  void  *dst;
  const void * src;
  size_t len;
}
CopyJob;

struct KVMGFXState
{
  bool                  running;
  SDL_Window          * window;
  SDL_Renderer        * renderer;
  struct KVMGFXHeader * shm;

  SDL_sem    * cpySem;
  SDL_Thread * cpyThreads[COPY_THREADS];
  CopyJob      cpyJobs   [COPY_THREADS];

#ifdef DEBUG_INPUT_STATE
  uint8_t kb[(SDL_NUM_SCANCODES / sizeof(uint8_t))+1];
  bool    mouse[10];
#endif
};

struct KVMGFXState state;

int copyThread(void * arg)
{
  CopyJob * job = (CopyJob *)arg;

  while(state.running)
  {
    SDL_LockMutex(job->mutex);
    while(!job->rdy)
      SDL_CondWait(job->cond, job->mutex);
    job->rdy = false;
    SDL_UnlockMutex(job->mutex);

    memcpy(job->dst, job->src, job->len);

    // return a lock to the pool
    SDL_SemPost(state.cpySem);
  }

  return 0;
}

bool startCopyThreads()
{
  state.cpySem = SDL_CreateSemaphore(COPY_THREADS);

  for(int i = 0; i < COPY_THREADS; ++i)
  {
    // take a lock from the pool
    SDL_SemWait(state.cpySem);

    CopyJob * job = &state.cpyJobs[i];
    job->mutex  = SDL_CreateMutex();
    job->cond   = SDL_CreateCond();
    job->rdy    = false;
    job->dst    = NULL;
    job->src    = NULL;
    job->len    = 0;

    job->thread = SDL_CreateThread(
        copyThread, "copyThread", &state.cpyJobs[i]);
  }

  return true;
}

void stopCopyThreads()
{
}

void compFunc_NONE(uint8_t * dst, const uint8_t * src, const size_t len)
{
  const size_t part = len / COPY_THREADS;
  for(int i = 0; i < COPY_THREADS; ++i)
  {
    CopyJob * job = &state.cpyJobs[i];
    job->dst = dst + i * part;
    job->src = src + i * part;
    job->len = part;
    job->rdy = true;
    SDL_CondSignal(job->cond);
  }

  // wait for the threads to complete
  for(int i = 0; i < COPY_THREADS; ++i)
    SDL_SemWait(state.cpySem);
}

void compFunc_BLACK_RLE(uint8_t * dst, const uint8_t * src, const size_t len)
{
  const size_t pixels = len / 3;
  for(size_t i = 0; i < pixels;)
  {
    if (!src[0] && !src[1] && !src[2])
    {
      struct RLEHeader * h = (struct RLEHeader *)src;
      dst += h->length * 3;
      i   += h->length;
      src += sizeof(struct RLEHeader);
      continue;
    }

    memcpy(dst, src, 3);
    dst += 3;
    src += 3;
    ++i;
  }
}

inline bool areFormatsSame(const struct KVMGFXHeader s1, const struct KVMGFXHeader s2)
{
  return
    (s1.version   == s2.version  ) &&
    (s1.frameType == s2.frameType) &&
    (s1.compType  == s2.compType ) &&
    (s1.width     == s2.width    ) &&
    (s1.height    == s2.height   );
}

void drawFunc_ARGB10(CompFunc compFunc, SDL_Texture * texture, uint8_t * dst, const uint8_t * src)
{
  SDL_UpdateTexture(texture, NULL, src, state.shm->stride * 4);
  ivshmem_kick_irq(state.shm->guestID, 0);

  SDL_RenderClear(state.renderer);

  SDL_RenderCopy(state.renderer, texture, NULL, NULL);
  SDL_RenderPresent(state.renderer);
}

void drawFunc_ARGB(CompFunc compFunc, SDL_Texture * texture, uint8_t * dst, const uint8_t * src)
{
  compFunc(dst, src, state.shm->height * state.shm->stride * 4);
  ivshmem_kick_irq(state.shm->guestID, 0);

  SDL_UnlockTexture(texture);
  SDL_RenderClear(state.renderer);

  SDL_RenderCopy(state.renderer, texture, NULL, NULL);
  SDL_RenderPresent(state.renderer);
}

void drawFunc_RGB(CompFunc compFunc, SDL_Texture * texture, uint8_t * dst, const uint8_t * src)
{
  compFunc(dst, src, state.shm->height * state.shm->stride * 3);
  ivshmem_kick_irq(state.shm->guestID, 0);

  SDL_UnlockTexture(texture);
  SDL_RenderClear(state.renderer);

  SDL_RenderCopy(state.renderer, texture, NULL, NULL);
  SDL_RenderPresent(state.renderer);
}

void drawFunc_XOR(CompFunc compFunc, SDL_Texture * texture, uint8_t * dst, const uint8_t * src)
{
  glEnable(GL_COLOR_LOGIC_OP);
  glLogicOp(GL_XOR);

  compFunc(dst, src, state.shm->height * state.shm->stride * 3);
  ivshmem_kick_irq(state.shm->guestID, 0);

  SDL_UnlockTexture(texture);
  if (state.shm->frames == 1)
    SDL_RenderClear(state.renderer);

  SDL_RenderCopy(state.renderer, texture, NULL, NULL);
  SDL_RenderPresent(state.renderer);

  // clear the buffer for the next frame
  memset(dst, 0, state.shm->height * state.shm->stride * 3);
}

void drawFunc_YUV420P(CompFunc compFunc, SDL_Texture * texture, uint8_t * dst, const uint8_t * src)
{
  const unsigned int pixels = state.shm->width * state.shm->height;

  SDL_UpdateYUVTexture(texture, NULL,
      src                      , state.shm->stride,
      src + pixels             , state.shm->stride / 2,
      src + pixels + pixels / 4, state.shm->stride / 2
  );

  ivshmem_kick_irq(state.shm->guestID, 0);

  SDL_RenderClear(state.renderer);
  SDL_RenderCopy(state.renderer, texture, NULL, NULL);
  SDL_RenderPresent(state.renderer);
}

int renderThread(void * unused)
{
  struct KVMGFXHeader format;
  SDL_Texture        *texture    = NULL;
  uint8_t             *pixels    = (uint8_t*)(state.shm + 1);
  uint8_t             *texPixels = NULL;
  DrawFunc            drawFunc   = NULL;
  CompFunc            compFunc   = NULL;

  format.version   = 1;
  format.frameType = FRAME_TYPE_INVALID;
  format.width     = 0;
  format.height    = 0;
  format.stride    = 0;
  format.frames    = 0;

  while(state.running)
  {
    // ensure the header magic is valid, this will help prevent crash out when the memory hasn't yet been initialized
    if (memcmp(state.shm->magic, KVMGFX_HEADER_MAGIC, sizeof(KVMGFX_HEADER_MAGIC)) != 0)
      continue;

    if (state.shm->version != 2)
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
    if (format.frameType == FRAME_TYPE_INVALID || !areFormatsSame(format, *state.shm))
    {
      if (texture)
      {
        SDL_DestroyTexture(texture);
        texture = NULL;
      }

      Uint32 sdlFormat;
      switch(state.shm->frameType)
      {
        case FRAME_TYPE_ARGB   : sdlFormat = SDL_PIXELFORMAT_ARGB8888   ; drawFunc = drawFunc_ARGB   ; break;
        case FRAME_TYPE_RGB    : sdlFormat = SDL_PIXELFORMAT_RGB24      ; drawFunc = drawFunc_RGB    ; break;
        case FRAME_TYPE_YUV420P: sdlFormat = SDL_PIXELFORMAT_YV12       ; drawFunc = drawFunc_YUV420P; break;
        case FRAME_TYPE_ARGB10 : sdlFormat = SDL_PIXELFORMAT_ARGB2101010; drawFunc = drawFunc_ARGB10 ; break;
        case FRAME_TYPE_XOR    : sdlFormat = SDL_PIXELFORMAT_RGB24      ; drawFunc = drawFunc_XOR    ; break;
        default:
          format.frameType = FRAME_TYPE_INVALID;
          continue;
      }

      switch(state.shm->compType)
      {
        case FRAME_COMP_NONE     : compFunc = compFunc_NONE     ; break;
        case FRAME_COMP_BLACK_RLE: compFunc = compFunc_BLACK_RLE; break;
        default:
          format.frameType = FRAME_TYPE_INVALID;
          continue;
      }

      // update the window size and create the render texture
      SDL_SetWindowSize(state.window, state.shm->width, state.shm->height);
      SDL_SetWindowPosition(state.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

      texture = SDL_CreateTexture(state.renderer, sdlFormat, SDL_TEXTUREACCESS_STREAMING, state.shm->width, state.shm->height);

      // this doesnt "lock" anything, pre-fetch the pointers for later use
      int unused;
      SDL_LockTexture(texture, NULL, (void**)&texPixels, &unused);

      memcpy(&format, state.shm, sizeof(format));
    }

    if (format.frames != state.shm->frames - 1)
      DEBUG_INFO("dropped %lu", state.shm->frames - format.frames);
    format.frames = state.shm->frames;

    glDisable(GL_COLOR_LOGIC_OP);
    drawFunc(compFunc, texture, texPixels, pixels);
    state.shm->clientFrame = format.frames;
  }

  SDL_DestroyTexture(texture);
  stopCopyThreads();
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
  int  mouseX      = 0;
  int  mouseY      = 0;
  int  repeatCount = 0;

  // ensure mouse acceleration is identical in server mode
  SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "1", SDL_HINT_OVERRIDE);

  // default to server mode
  bool serverMode = true;
  spice_mouse_mode(true);
  SDL_SetRelativeMouseMode(true);

  SDL_Event event;
  while(state.running)
  {
    while(SDL_PollEvent(&event))
    {
      switch(event.type)
      {
        case SDL_QUIT:
          state.running = false;
          break;

        case SDL_KEYDOWN:
        {
          SDL_Scancode sc = event.key.keysym.scancode;
          if (event.key.repeat)
            ++repeatCount;
          else
            repeatCount = 0;

          if (sc == SDL_SCANCODE_SCROLLLOCK)
          {
            if (event.key.repeat)
              break;

            serverMode = !serverMode;
            spice_mouse_mode(serverMode);
            SDL_SetRelativeMouseMode(serverMode);

#ifdef DEBUG_INPUT_STATE
            DEBUG_INFO("mouse state:");
            for (unsigned int i = 0; i < sizeof(state.mouse) / sizeof(bool); ++i)
            {
              if (state.mouse[i])
                DEBUG_INFO("0x%02x", i);
            }

            DEBUG_INFO("keyboard state:");
            for (unsigned int i = 0; i < SDL_NUM_SCANCODES; ++i)
            {
              unsigned int block = i / 8;
              if (state.kb[block] & (1 << (i - block * 8)))
                DEBUG_INFO("0x%02x", i);
            }
#endif
            break;
          }

          if (repeatCount != 0 && repeatCount != 100)
            break;

          repeatCount = 0;
          uint32_t scancode = mapScancode(sc);
          if (scancode == 0)
            break;

#ifdef DEBUG_INPUT_STATE
          uint16_t block = scancode / 8;
          state.kb[block] |= 1 << (scancode - block * 8);
#endif

          spice_key_down(scancode);
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

#ifdef DEBUG_INPUT_STATE
          uint16_t block = scancode / 8;
          state.kb[block] &= ~(1 << (scancode - block * 8));
#endif

          spice_key_up(scancode);
          break;
        }

        case SDL_MOUSEWHEEL:
          spice_mouse_press  (event.wheel.y == 1 ? 4 : 5);
          spice_mouse_release(event.wheel.y == 1 ? 4 : 5);
          break;

        case SDL_MOUSEMOTION:
          if (serverMode)
            spice_mouse_motion(event.motion.xrel, event.motion.yrel);
          else
            spice_mouse_motion(
                (int)event.motion.x - mouseX,
                (int)event.motion.y - mouseY
            );

          mouseX = event.motion.x;
          mouseY = event.motion.y;
          break;

        case SDL_MOUSEBUTTONDOWN:
#ifdef DEBUG_INPUT_STATE
          state.mouse[event.button.button] = true;
#endif
          spice_mouse_position(event.button.x, event.button.y);
          spice_mouse_press(event.button.button);
          break;

        case SDL_MOUSEBUTTONUP:
#ifdef DEBUG_INPUT_STATE
          state.mouse[event.button.button] = false;
#endif
          spice_mouse_position(event.button.x, event.button.y);
          spice_mouse_release(event.button.button);
          break;

        default:
          break;
      }
    }
    usleep(1000);
  }

  return 0;
}

int main(int argc, char * argv[])
{
  memset(&state, 0, sizeof(state));
  state.running = true;

  if (SDL_Init(SDL_INIT_VIDEO) < 0)
  {
    DEBUG_ERROR("SDL_Init Failed");
    return -1;
  }

  state.window = SDL_CreateWindow("KVM-GFX Test", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 100, 100, SDL_WINDOW_BORDERLESS);
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

  if (!state.renderer)
  {
    DEBUG_ERROR("failed to create window");
    return -1;
  }

  startCopyThreads();


  int         shm_fd    = 0;
  SDL_Thread *t_ivshmem = NULL;
  SDL_Thread *t_spice   = NULL;
  SDL_Thread *t_event   = NULL;

  while(1)
  {
    if (!ivshmem_connect("/tmp/ivshmem_socket"))
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

    if (!spice_connect("127.0.0.1", 5900, ""))
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