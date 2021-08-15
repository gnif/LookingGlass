/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#define _GNU_SOURCE //needed for pthread_setname_np

#include <obs/obs-config.h>
#include <obs/obs-module.h>
#include <obs/util/threading.h>
#include <obs/graphics/graphics.h>
#include <obs/graphics/matrix4.h>

#include <common/array.h>
#include <common/ivshmem.h>
#include <common/KVMFR.h>
#include <common/framebuffer.h>
#include <lgmp/client.h>

#include <stdio.h>
#include <unistd.h>
#include <stdatomic.h>
#include <GL/gl.h>

/**
 * the following comes from drm_fourcc.h and is included here to avoid the
 * external dependency for the few simple defines we need
 */
#define fourcc_code(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
         ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_FORMAT_ARGB8888      fourcc_code('A', 'R', '2', '4')
#define DRM_FORMAT_ABGR8888      fourcc_code('A', 'B', '2', '4')
#define DRM_FORMAT_BGRA1010102   fourcc_code('B', 'A', '3', '0')
#define DRM_FORMAT_ABGR16161616F fourcc_code('A', 'B', '4', 'H')

typedef enum
{
  STATE_STOPPED,
  STATE_OPEN,
  STATE_STARTING,
  STATE_RUNNING,
  STATE_STOPPING
}
LGState;

typedef struct
{
  KVMFRFrame * frame;
  size_t       dataSize;
  int          fd;
}
DMAFrameInfo;

typedef struct
{
  obs_source_t    * context;
  LGState           state;
  char            * shmFile;
  uint32_t          formatVer;
  uint32_t          width, height;
  FrameType         type;
  int               bpp;
  struct IVSHMEM    shmDev;
  PLGMPClient       lgmp;
  PLGMPClientQueue  frameQueue, pointerQueue;
  gs_texture_t    * texture;
  uint8_t         * texData;
  uint32_t          linesize;

#if LIBOBS_API_MAJOR_VER >= 27
  bool              dmabuf;
  DMAFrameInfo      dmaInfo[LGMP_Q_FRAME_LEN];
#endif

  pthread_t         frameThread, pointerThread;
  os_sem_t        * frameSem;

  bool                 cursorMono;
  gs_texture_t       * cursorTex;
  struct gs_rect       cursorRect;

  bool                 cursorVisible;
  KVMFRCursor          cursor;
  os_sem_t           * cursorSem;
  atomic_uint          cursorVer;
  unsigned int         cursorCurVer;
  uint32_t             cursorSize;
  uint32_t           * cursorData;
}
LGPlugin;

static void lgUpdate(void * data, obs_data_t * settings);

static const char * lgGetName(void * unused)
{
  return obs_module_text("Looking Glass Client");
}

static void * lgCreate(obs_data_t * settings, obs_source_t * context)
{
  LGPlugin * this = bzalloc(sizeof(LGPlugin));
  this->context = context;
  os_sem_init (&this->frameSem , 0);
  os_sem_init (&this->cursorSem, 1);
  atomic_store(&this->cursorVer, 0);
  lgUpdate(this, settings);
  return this;
}

static void deinit(LGPlugin * this)
{
  switch(this->state)
  {
    case STATE_STARTING:
      /* wait for startup to finish */
      while(this->state == STATE_STARTING)
        usleep(1);
      /* fallthrough */

    case STATE_RUNNING:
    case STATE_STOPPING:
      this->state = STATE_STOPPING;
      pthread_join(this->frameThread  , NULL);
      pthread_join(this->pointerThread, NULL);
      this->state = STATE_STOPPED;
      /* fallthrough */

    case STATE_OPEN:
#if LIBOBS_API_MAJOR_VER >= 27
      for (int i = 0 ; i < ARRAY_LENGTH(this->dmaInfo); ++i)
        if (this->dmaInfo[i].fd >= 0)
        {
          close(this->dmaInfo[i].fd);
          this->dmaInfo[i].fd = -1;
        }
#endif

      lgmpClientFree(&this->lgmp);
      ivshmemClose(&this->shmDev);
      break;

    case STATE_STOPPED:
      break;
  }

  if (this->shmFile)
  {
    bfree(this->shmFile);
    this->shmFile = NULL;
  }

  if (this->texture)
  {
    obs_enter_graphics();
    gs_texture_destroy(this->texture);
    gs_texture_unmap(this->texture);
    obs_leave_graphics();
    this->texture = NULL;
  }

  if (this->cursorTex)
  {
    obs_enter_graphics();
    gs_texture_destroy(this->cursorTex);
    gs_texture_unmap(this->cursorTex);
    obs_leave_graphics();
    this->cursorTex = NULL;
  }

  this->state = STATE_STOPPED;
}

static void lgDestroy(void * data)
{
  LGPlugin * this = (LGPlugin *)data;
  deinit(this);
  os_sem_destroy(this->frameSem );
  os_sem_destroy(this->cursorSem);
  bfree(this);
}

static void lgGetDefaults(obs_data_t * defaults)
{
  obs_data_set_default_string(defaults, "shmFile", "/dev/shm/looking-glass");
}

static obs_properties_t * lgGetProperties(void * data)
{
  obs_properties_t * props = obs_properties_create();

  obs_properties_add_text(props, "shmFile", obs_module_text("SHM File"), OBS_TEXT_DEFAULT);
#if LIBOBS_API_MAJOR_VER >= 27
  obs_properties_add_bool(props, "dmabuf",  obs_module_text("Use DMABUF import (requires kvmfr device)"));
#else
  obs_property_t * dmabuf = obs_properties_add_bool(props, "dmabuf",
      obs_module_text("Use DMABUF import (requires OBS 27+ and kvmfr device)"));
  obs_property_set_enabled(dmabuf, false);
#endif

  return props;
}

static void * frameThread(void * data)
{
  LGPlugin * this = (LGPlugin *)data;

  if (lgmpClientSubscribe(this->lgmp, LGMP_Q_FRAME, &this->frameQueue) != LGMP_OK)
  {
    this->state = STATE_STOPPING;
    return NULL;
  }

  this->state = STATE_RUNNING;
  os_sem_post(this->frameSem);

  while(this->state == STATE_RUNNING)
  {
    LGMP_STATUS status;

    os_sem_wait(this->frameSem);
    if ((status = lgmpClientAdvanceToLast(this->frameQueue)) != LGMP_OK)
    {
      if (status != LGMP_ERR_QUEUE_EMPTY)
      {
        os_sem_post(this->frameSem);
        printf("lgmpClientAdvanceToLast: %s\n", lgmpStatusString(status));
        break;
      }
    }
    os_sem_post(this->frameSem);
    usleep(1000);
  }

  lgmpClientUnsubscribe(&this->frameQueue);
  this->state = STATE_STOPPING;
  return NULL;
}

inline static void allocCursorData(LGPlugin * this, const unsigned int size)
{
  if (this->cursorSize >= size)
    return;

  bfree(this->cursorData);
  this->cursorSize = size;
  this->cursorData = bmalloc(size);
}

static void * pointerThread(void * data)
{
  LGPlugin * this = (LGPlugin *)data;

  if (lgmpClientSubscribe(this->lgmp, LGMP_Q_POINTER, &this->pointerQueue) != LGMP_OK)
  {
    this->state = STATE_STOPPING;
    return NULL;
  }

  while(this->state == STATE_RUNNING)
  {
    LGMP_STATUS status;
    LGMPMessage msg;

    if ((status = lgmpClientProcess(this->pointerQueue, &msg)) != LGMP_OK)
    {
      if (status != LGMP_ERR_QUEUE_EMPTY)
      {
        printf("lgmpClientProcess: %s\n", lgmpStatusString(status));
        break;
      }

      usleep(1000);
      continue;
    }

    const KVMFRCursor * const cursor = (const KVMFRCursor * const)msg.mem;
    this->cursorVisible =
      msg.udata & CURSOR_FLAG_VISIBLE;

    if (msg.udata & CURSOR_FLAG_SHAPE)
    {
      os_sem_wait(this->cursorSem);
      const uint8_t * const data = (const uint8_t * const)(cursor + 1);
      unsigned int dataSize = 0;

      switch(cursor->type)
      {
        case CURSOR_TYPE_MASKED_COLOR:
        {
          dataSize = cursor->height * cursor->pitch;
          allocCursorData(this, dataSize);

          const uint32_t * s = (const uint32_t *)data;
          uint32_t * d       = this->cursorData;
          for(int i = 0; i < dataSize / sizeof(uint32_t); ++i, ++s, ++d)
            *d = (*s & ~0xFF000000) | (*s & 0xFF000000 ? 0x0 : 0xFF000000);
          break;
        }

        case CURSOR_TYPE_COLOR:
        {
          dataSize = cursor->height * cursor->pitch;
          allocCursorData(this, dataSize);
          memcpy(this->cursorData, data, dataSize);
          break;
        }

        case CURSOR_TYPE_MONOCHROME:
        {
          dataSize = cursor->height * cursor->width * sizeof(uint32_t);
          allocCursorData(this, dataSize);

          const int hheight = cursor->height / 2;
          uint32_t * d = this->cursorData;
          for(int y = 0; y < hheight; ++y)
            for(int x = 0; x < cursor->width; ++x)
            {
              const uint8_t  * srcAnd  = data   + (cursor->pitch * y) + (x / 8);
              const uint8_t  * srcXor  = srcAnd + cursor->pitch * hheight;
              const uint8_t    mask    = 0x80 >> (x % 8);
              const uint32_t   andMask = (*srcAnd & mask) ? 0xFFFFFFFF : 0xFF000000;
              const uint32_t   xorMask = (*srcXor & mask) ? 0x00FFFFFF : 0x00000000;

              d[y * cursor->width + x                          ] = andMask;
              d[y * cursor->width + x + cursor->width * hheight] = xorMask;
            }

          break;
        }

        default:
          printf("Invalid cursor type\n");
          break;
      }

      this->cursor.type   = cursor->type;
      this->cursor.width  = cursor->width;
      this->cursor.height = cursor->height;

      atomic_fetch_add_explicit(&this->cursorVer, 1, memory_order_relaxed);
      os_sem_post(this->cursorSem);
    }

    if (msg.udata & CURSOR_FLAG_POSITION)
    {
      this->cursor.x = cursor->x;
      this->cursor.y = cursor->y;
    }

    lgmpClientMessageDone(this->pointerQueue);
  }

  lgmpClientUnsubscribe(&this->pointerQueue);

  bfree(this->cursorData);
  this->cursorData = NULL;
  this->cursorSize = 0;

  this->state = STATE_STOPPING;
  return NULL;
}

static void lgUpdate(void * data, obs_data_t * settings)
{
  LGPlugin * this = (LGPlugin *)data;

  deinit(this);
  this->shmFile = bstrdup(obs_data_get_string(settings, "shmFile"));
  if (!ivshmemOpenDev(&this->shmDev, this->shmFile))
    return;

#if LIBOBS_API_MAJOR_VER >= 27
  this->dmabuf = obs_data_get_bool(settings, "dmabuf") && ivshmemHasDMA(&this->shmDev);
#endif

  this->state = STATE_OPEN;

  uint32_t udataSize;
  KVMFR * udata;

  if (lgmpClientInit(this->shmDev.mem, this->shmDev.size, &this->lgmp)
      != LGMP_OK)
    return;

  usleep(200000);

  if (lgmpClientSessionInit(this->lgmp, &udataSize, (uint8_t **)&udata)
      != LGMP_OK)
    return;

  if (udataSize != sizeof(KVMFR) ||
      memcmp(udata->magic, KVMFR_MAGIC, sizeof(udata->magic)) != 0 ||
      udata->version != KVMFR_VERSION)
  {
    printf("The host application is not compatible with this client\n");
    printf("Expected KVMFR version %d\n", KVMFR_VERSION);
    printf("This is not a Looking Glass error, do not report this\n");
    return;
  }

  this->state = STATE_STARTING;
  pthread_create(&this->frameThread, NULL, frameThread, this);
  pthread_setname_np(this->frameThread, "LGFrameThread");
  pthread_create(&this->pointerThread, NULL, pointerThread, this);
  pthread_setname_np(this->pointerThread, "LGPointerThread");
}

#if LIBOBS_API_MAJOR_VER >= 27
static int dmabufGetFd(LGPlugin * this, LGMPMessage * msg, KVMFRFrame * frame, size_t dataSize)
{
  DMAFrameInfo * dma = NULL;

  /* find the existing dma buffer if it exists */
  for (int i = 0; i < ARRAY_LENGTH(this->dmaInfo); ++i)
    if (this->dmaInfo[i].frame == frame)
    {
      dma = this->dmaInfo + i;
      /* if it's too small close it */
      if (dma->dataSize < dataSize)
      {
        close(dma->fd);
        dma->fd = -1;
      }
      break;
    }

  /* otherwise find a free buffer for use */
  if (!dma)
    for (int i = 0; i < ARRAY_LENGTH(this->dmaInfo); ++i)
      if (!this->dmaInfo[i].frame)
      {
        dma = this->dmaInfo + i;
        dma->frame = frame;
        dma->fd    = -1;
        break;
      }

  assert(dma);

  /* open the buffer */
  if (dma->fd == -1)
  {
    const uintptr_t pos    = (uintptr_t) msg->mem - (uintptr_t) this->shmDev.mem;
    const uintptr_t offset = (uintptr_t) frame->offset + FrameBufferStructSize;

    dma->dataSize = dataSize;
    dma->fd       = ivshmemGetDMABuf(&this->shmDev, pos + offset, dataSize);
    if (dma->fd < 0)
    {
      puts("Failed to get the DMA buffer for the frame");
      return -1;
    }
  }

  return dma->fd;
}
#endif

static void lgVideoTick(void * data, float seconds)
{
  LGPlugin * this = (LGPlugin *)data;

  if (this->state != STATE_RUNNING)
    return;

  LGMP_STATUS status;
  LGMPMessage msg;
  bool framebuffer = true;

  os_sem_wait(this->frameSem);
  if (this->state != STATE_RUNNING)
  {
    os_sem_post(this->frameSem);
    return;
  }

  this->cursorRect.x = this->cursor.x;
  this->cursorRect.y = this->cursor.y;

  /* update the cursor texture */
  unsigned int cursorVer = atomic_load(&this->cursorVer);
  if (cursorVer != this->cursorCurVer)
  {
    os_sem_wait(this->cursorSem);
    obs_enter_graphics();

    if (this->cursorTex)
    {
      gs_texture_destroy(this->cursorTex);
      this->cursorTex = NULL;
    }

    switch(this->cursor.type)
    {
      case CURSOR_TYPE_MASKED_COLOR:
        /* fallthrough */

      case CURSOR_TYPE_COLOR:
        this->cursorMono  = false;
        this->cursorTex   =
          gs_texture_create(
              this->cursor.width,
              this->cursor.height,
              GS_BGRA,
              1,
              (const uint8_t **)&this->cursorData,
              GS_DYNAMIC);
        break;

      case CURSOR_TYPE_MONOCHROME:
        this->cursorMono = true;
        this->cursorTex  =
          gs_texture_create(
              this->cursor.width,
              this->cursor.height,
              GS_BGRA,
              1,
              (const uint8_t **)&this->cursorData,
              GS_DYNAMIC);
        break;

      default:
        break;
    }

    obs_leave_graphics();

    this->cursorCurVer  = cursorVer;
    this->cursorRect.cx = this->cursor.width;
    this->cursorRect.cy = this->cursor.height;

    os_sem_post(this->cursorSem);
  }

  if ((status = lgmpClientAdvanceToLast(this->frameQueue)) != LGMP_OK)
  {
    if (status != LGMP_ERR_QUEUE_EMPTY)
    {
      os_sem_post(this->frameSem);
      printf("lgmpClientAdvanceToLast: %s\n", lgmpStatusString(status));
      return;
    }
  }

  if ((status = lgmpClientProcess(this->frameQueue, &msg)) != LGMP_OK)
  {
    if (status == LGMP_ERR_QUEUE_EMPTY)
    {
      os_sem_post(this->frameSem);
      return;
    }

    printf("lgmpClientProcess: %s\n", lgmpStatusString(status));
    this->state = STATE_STOPPING;
    os_sem_post(this->frameSem);
    return;
  }

  KVMFRFrame * frame = (KVMFRFrame *)msg.mem;
  if (!this->texture || this->formatVer != frame->formatVer)
  {
    this->formatVer = frame->formatVer;
    this->width     = frame->width;
    this->height    = frame->height;
    this->type      = frame->type;

    obs_enter_graphics();
    if (this->texture)
    {
      gs_texture_unmap(this->texture);
      gs_texture_destroy(this->texture);
      this->texture = NULL;
    }

    enum gs_color_format format;
    uint32_t drm_format;

    this->bpp = 4;
    switch(this->type)
    {
      case FRAME_TYPE_BGRA:
        format     = GS_BGRA;
        drm_format = DRM_FORMAT_ARGB8888;
        break;

      case FRAME_TYPE_RGBA:
        format     = GS_RGBA;
        drm_format = DRM_FORMAT_ARGB8888;
        break;

      case FRAME_TYPE_RGBA10:
        format     = GS_R10G10B10A2;
        drm_format = DRM_FORMAT_BGRA1010102;
        break;

      case FRAME_TYPE_RGBA16F:
        this->bpp  = 8;
        format     = GS_RGBA16F;
        drm_format = DRM_FORMAT_ABGR16161616F;
        break;

      default:
        printf("invalid type %d\n", this->type);
        os_sem_post(this->frameSem);
        obs_leave_graphics();
        return;
    }

    this->texture = NULL;

#if LIBOBS_API_MAJOR_VER >= 27
    if (this->dmabuf)
    {
      int fd = dmabufGetFd(this, &msg, frame, frame->height * frame->pitch);

      if (fd < 0)
        goto dmabuf_fail;

      this->texture = gs_texture_create_from_dmabuf(frame->width, frame->height,
        drm_format, format, 1, &fd, &(uint32_t) { frame->pitch },
        &(uint32_t) { 0 }, &(uint64_t) { 0 });

      if (!this->texture)
      {
        puts("Failed to create dmabuf texture");
        this->dmabuf = false;
        goto dmabuf_fail;
      }

      framebuffer = false;
    }

  dmabuf_fail:
#else
    (void) drm_format;
#endif

    if (!this->texture)
      this->texture = gs_texture_create(
          this->width, this->height, format, 1, NULL, GS_DYNAMIC);

    if (!this->texture)
    {
      printf("create texture failed\n");
      os_sem_post(this->frameSem);
      obs_leave_graphics();
      return;
    }

    gs_texture_map(this->texture, &this->texData, &this->linesize);
    obs_leave_graphics();
  }

  if (framebuffer && this->texture)
  {
    FrameBuffer * fb = (FrameBuffer *)(((uint8_t*)frame) + frame->offset);
    framebuffer_read(
        fb,
        this->texData,    // dst
        this->linesize,   // dstpitch
        frame->height,    // height
        frame->width,     // width
        this->bpp,        // bpp
        frame->pitch      // linepitch
    );

    lgmpClientMessageDone(this->frameQueue);
    os_sem_post(this->frameSem);

    obs_enter_graphics();
    gs_texture_unmap(this->texture);
    gs_texture_map(this->texture, &this->texData, &this->linesize);
    obs_leave_graphics();
  }
  else
  {
    lgmpClientMessageDone(this->frameQueue);
    os_sem_post(this->frameSem);
  }
}

static void lgVideoRender(void * data, gs_effect_t * effect)
{
  LGPlugin * this = (LGPlugin *)data;

  if (!this->texture)
    return;

  effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);
  gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
  gs_effect_set_texture(image, this->texture);

  while (gs_effect_loop(effect, "Draw"))
    gs_draw_sprite(this->texture, 0, 0, 0);

  if (this->cursorVisible && this->cursorTex)
  {
    struct matrix4 m4;
    gs_matrix_get(&m4);
    struct gs_rect r =
    {
      .x  = m4.t.x,
      .y  = m4.t.y,
      .cx = (double)this->width  * m4.x.x,
      .cy = (double)this->height * m4.y.y
    };
    gs_set_scissor_rect(&r);

    effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    image  = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture(image, this->cursorTex);

    gs_matrix_push();
    gs_matrix_translate3f(this->cursorRect.x, this->cursorRect.y, 0.0f);

    if (!this->cursorMono)
    {
      gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
      while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(this->cursorTex, 0, 0, 0);
      gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
    }
    else
    {
      while (gs_effect_loop(effect, "Draw"))
      {
        glEnable(GL_COLOR_LOGIC_OP);

        glLogicOp(GL_AND);
        gs_draw_sprite_subregion(
            this->cursorTex    , 0,
            0                  , 0,
            this->cursorRect.cx, this->cursorRect.cy / 2);

        glLogicOp(GL_XOR);
        gs_draw_sprite_subregion(
            this->cursorTex    , 0,
            0                  , this->cursorRect.cy / 2,
            this->cursorRect.cx, this->cursorRect.cy / 2);

        glDisable(GL_COLOR_LOGIC_OP);
      }
    }

    gs_matrix_pop();
    gs_set_scissor_rect(NULL);
  }
}

static uint32_t lgGetWidth(void * data)
{
  LGPlugin * this = (LGPlugin *)data;
  return this->width;
}

static uint32_t lgGetHeight(void * data)
{
  LGPlugin * this = (LGPlugin *)data;
  return this->height;
}

struct obs_source_info lg_source =
{
  .id             = "looking-glass-obs",
  .type           = OBS_SOURCE_TYPE_INPUT,
  .output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
                    OBS_SOURCE_DO_NOT_DUPLICATE,
  .get_name       = lgGetName,
  .create         = lgCreate,
  .destroy        = lgDestroy,
  .update         = lgUpdate,
  .get_defaults   = lgGetDefaults,
  .get_properties = lgGetProperties,
  .video_tick     = lgVideoTick,
  .video_render   = lgVideoRender,
  .get_width      = lgGetWidth,
  .get_height     = lgGetHeight,
//  .icon_type      = OBS_ICON_TYPE_DESKTOP_CAPTURE
};
