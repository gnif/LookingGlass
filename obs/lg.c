/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
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

#include "rgb24.effect.h"
#include "hdrpq.effect.h"

/* scRGB reference white in cd/m² (OBS GS_CS_709_SCRGB is defined as
 * 1.0 = 80 cd/m²). PQ encodes an absolute 0..10000 cd/m² range, so linear
 * PQ output is scaled by 10000 / 80 to reach the scRGB convention. */
#define LG_SCRGB_REFERENCE_WHITE 80.0f

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
  STATE_STOPPING,
  STATE_RESTARTING
}
LGState;

#if LIBOBS_API_MAJOR_VER >= 27
typedef struct
{
  const KVMFRFrame * frame;
  size_t             dataSize;
  int                fd;
  gs_texture_t     * texture;
}
DMAFrameInfo;
#endif

typedef struct
{
  obs_source_t       * context;
  LGState              state;
  char               * shmFile;
  uint32_t             formatVer;
  uint32_t             screenWidth, screenHeight;
  uint32_t             dataWidth, dataHeight;
  uint32_t             frameWidth, frameHeight;
  enum gs_color_format format;
  bool                 unpack;
  uint32_t             drmFormat;
  struct vec2          screenScale;
  FrameType            type;
  int                  bpp;
  struct IVSHMEM       shmDev;
  PLGMPClient          lgmp;
  PLGMPClientQueue     frameQueue, pointerQueue;
  gs_texture_t       * texture;
  gs_texture_t       * dstTexture;
  uint8_t            * texData;
  uint32_t             linesize;

  bool                 hideMouse;
  bool                 hdr;
  bool                 hdrPQ;
  struct vec3          colorMatrix[3]; // source primaries -> BT.709 (linear)
  float                hdrScale;       // scRGB reference white scaling
#if LIBOBS_API_MAJOR_VER >= 27
  bool                 dmabuf;
  bool                 dmabufTested;
  DMAFrameInfo         dmaInfo[LGMP_Q_FRAME_LEN];
  gs_texture_t       * dmaTexture;
#endif

#if LIBOBS_API_MAJOR_VER >= 28
  enum gs_color_space colorSpace;
#endif

  pthread_t            frameThread, pointerThread;
  os_sem_t           * frameSem;

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

  gs_effect_t * unpackEffect;
  gs_eparam_t * image;
  gs_eparam_t * outputSize;
  gs_eparam_t * swap;

  gs_effect_t * hdrEffect;
  gs_eparam_t * hdrImage;
  gs_eparam_t * hdrOutputSize;
  gs_eparam_t * hdrColorMatrix[3];
  gs_eparam_t * hdrScaleParam;
}
LGPlugin;

static void * frameThread(void * data);
static void * pointerThread(void * data);
static void lgUpdate(void * data, obs_data_t * settings);

static const char * lgGetName(void * unused)
{
  return obs_module_text("Looking Glass Client");
}

static void * lgCreate(obs_data_t * settings, obs_source_t * context)
{
  LGPlugin * this = bzalloc(sizeof(LGPlugin));

  this->context = context;

  obs_enter_graphics();
  char * error = NULL;
  this->unpackEffect = gs_effect_create(b_effect_rgb24_effect, "rbg24", &error);
  if (!this->unpackEffect)
  {
    blog(LOG_ERROR, "%s", error);
    bfree(error);
    bfree(this);
    obs_leave_graphics();
    return NULL;
  }

  this->image      = gs_effect_get_param_by_name(
      this->unpackEffect, "image"     );
  this->outputSize = gs_effect_get_param_by_name(
      this->unpackEffect, "outputSize");
  this->swap       = gs_effect_get_param_by_name(
      this->unpackEffect, "swap"      );

  this->hdrEffect = gs_effect_create(b_effect_hdrpq_effect, "hdrpq", &error);
  if (!this->hdrEffect)
  {
    blog(LOG_ERROR, "%s", error);
    bfree(error);
    gs_effect_destroy(this->unpackEffect);
    bfree(this);
    obs_leave_graphics();
    return NULL;
  }

  this->hdrImage         = gs_effect_get_param_by_name(
      this->hdrEffect, "image"       );
  this->hdrOutputSize    = gs_effect_get_param_by_name(
      this->hdrEffect, "outputSize"  );
  this->hdrColorMatrix[0] = gs_effect_get_param_by_name(
      this->hdrEffect, "colorMatrix0");
  this->hdrColorMatrix[1] = gs_effect_get_param_by_name(
      this->hdrEffect, "colorMatrix1");
  this->hdrColorMatrix[2] = gs_effect_get_param_by_name(
      this->hdrEffect, "colorMatrix2");
  this->hdrScaleParam    = gs_effect_get_param_by_name(
      this->hdrEffect, "scale"       );
  obs_leave_graphics();

  os_sem_init (&this->frameSem , 0);
  os_sem_init (&this->cursorSem, 1);
  atomic_store(&this->cursorVer, 0);
  lgUpdate(this, settings);
  return this;
}

static void createThreads(LGPlugin * this)
{
  pthread_create(&this->frameThread, NULL, frameThread, this);
  pthread_setname_np(this->frameThread, "LGFrameThread");
  pthread_create(&this->pointerThread, NULL, pointerThread, this);
  pthread_setname_np(this->pointerThread, "LGPointerThread");
}

static void waitThreads(LGPlugin * this)
{
  pthread_join(this->frameThread,   NULL);
  pthread_join(this->pointerThread, NULL);
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
    case STATE_RESTARTING:
      this->state = STATE_STOPPING;
      waitThreads(this);
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

  obs_enter_graphics();
  if (this->unpack && this->dstTexture)
  {
    gs_texture_destroy(this->dstTexture);
    this->dstTexture = NULL;
    this->unpack     = false;
  }

  if (this->texture)
  {
    if (!this->dmabuf)
      gs_texture_unmap(this->texture);
    gs_texture_destroy(this->texture);
    this->texture = NULL;
  }

  if (this->cursorTex)
  {
    gs_texture_destroy(this->cursorTex);
    this->cursorTex = NULL;
  }

#if LIBOBS_API_MAJOR_VER >= 27
  for (int i = 0 ; i < ARRAY_LENGTH(this->dmaInfo); ++i)
    if (this->dmaInfo[i].texture)
    {
      gs_texture_destroy(this->dmaInfo[i].texture);
      this->dmaInfo[i].texture = NULL;
    }
  this->dmaTexture = NULL;
#endif

  obs_leave_graphics();

  this->state = STATE_STOPPED;
}

static void lgDestroy(void * data)
{
  LGPlugin * this = (LGPlugin *)data;
  deinit(this);
  os_sem_destroy(this->frameSem );
  os_sem_destroy(this->cursorSem);

  obs_enter_graphics();
  gs_effect_destroy(this->unpackEffect);
  gs_effect_destroy(this->hdrEffect);
  obs_leave_graphics();

  bfree(this);
}

static void lgGetDefaults(obs_data_t * defaults)
{
  obs_data_set_default_string(defaults, "shmFile", "/dev/kvmfr0");
#if LIBOBS_API_MAJOR_VER >= 27
  obs_data_set_default_bool(defaults, "dmabuf", true);
#endif
}

static obs_properties_t * lgGetProperties(void * data)
{
  obs_properties_t * props = obs_properties_create();

  obs_properties_add_text(props, "shmFile",
      obs_module_text("SHM File"), OBS_TEXT_DEFAULT);
  obs_properties_add_bool(props, "hideMouse",
      obs_module_text("Hide mouse cursor"));
#if LIBOBS_API_MAJOR_VER >= 27
  obs_properties_add_bool(props, "dmabuf",
      obs_module_text("Use DMABUF import (requires kvmfr device)"));
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

  if (lgmpClientSubscribe(
        this->lgmp, LGMP_Q_FRAME, &this->frameQueue) != LGMP_OK)
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
  this->state = STATE_RESTARTING;
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

  if (lgmpClientSubscribe(
        this->lgmp, LGMP_Q_POINTER, &this->pointerQueue) != LGMP_OK)
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
    this->cursorVisible = this->hideMouse ?
      0 : msg.udata & CURSOR_FLAG_VISIBLE;

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
              const uint32_t   andMask = (*srcAnd & mask) ?
                0xFFFFFFFF : 0xFF000000;
              const uint32_t   xorMask = (*srcXor & mask) ?
                0x00FFFFFF : 0x00000000;

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

  this->state = STATE_RESTARTING;
  return NULL;
}

static void lgUpdate(void * data, obs_data_t * settings)
{
  LGPlugin * this = (LGPlugin *)data;

  deinit(this);
  this->shmFile = bstrdup(obs_data_get_string(settings, "shmFile"));
  if (!ivshmemOpenDev(&this->shmDev, this->shmFile))
    return;

  this->hideMouse = obs_data_get_bool(settings, "hideMouse") ? 1 : 0;
#if LIBOBS_API_MAJOR_VER >= 27
  this->dmabuf = obs_data_get_bool(settings, "dmabuf") &&
    ivshmemHasDMA(&this->shmDev);
#endif

  this->state = STATE_OPEN;

  uint32_t udataSize;
  KVMFR * udata;

  LGMP_STATUS status;
  if ((status = lgmpClientInit(this->shmDev.mem, this->shmDev.size,
      &this->lgmp)) != LGMP_OK)
  {
    printf("lgmpClientInit: %s\n", lgmpStatusString(status));
    return;
  }

  usleep(200000);

  uint32_t remoteVersion;
  if ((status = lgmpClientSessionInit(this->lgmp, &udataSize,
      (uint8_t **)&udata, NULL, &remoteVersion)) != LGMP_OK)
  {
    printf("lgmpClientSessionInit: %s", lgmpStatusString(status));
    if (status == LGMP_ERR_INVALID_VERSION)
    {
      printf("The host application is not compatible with this client\n");
      printf("Expected LGMP version %u but got %u\n",
          LGMP_PROTOCOL_VERSION, remoteVersion);
      printf("This is not a Looking Glass error, do not report this");
    }
    printf("\n");
    return;
  }

  if (udataSize < sizeof(KVMFR) ||
      memcmp(udata->magic, KVMFR_MAGIC, sizeof(udata->magic)) != 0 ||
      udata->version != KVMFR_VERSION)
  {
    printf("The host application is not compatible with this client\n");
    printf("Expected KVMFR version %d\n", KVMFR_VERSION);
    printf("This is not a Looking Glass error, do not report this\n");
    return;
  }

  this->state = STATE_STARTING;
  createThreads(this);
}

#if LIBOBS_API_MAJOR_VER >= 27
static DMAFrameInfo * dmabufOpenDMAFrameInfo(LGPlugin * this, LGMPMessage * msg,
    const KVMFRFrame * frame, size_t dataSize)
{
  DMAFrameInfo * fi = NULL;

  /* find the existing dma buffer if it exists */
  for (int i = 0; i < ARRAY_LENGTH(this->dmaInfo); ++i)
    if (this->dmaInfo[i].frame == frame)
      fi = &this->dmaInfo[i];

  /* if it's too small close it */
  if (fi && fi->dataSize < dataSize)
  {
    if (fi->texture)
    {
      gs_texture_destroy(fi->texture);
      fi->texture = NULL;
    }
    close(fi->fd);
    fi->fd = -1;
  }

  /* otherwise find a free buffer for use */
  if (!fi)
    for (int i = 0; i < ARRAY_LENGTH(this->dmaInfo); ++i)
      if (!this->dmaInfo[i].frame)
      {
        fi = &this->dmaInfo[i];
        fi->frame = frame;
        fi->fd    = -1;
        break;
      }

  assert(fi);

  /* open the buffer */
  if (fi->fd == -1)
  {
    const uintptr_t pos    = (uintptr_t) msg->mem - (uintptr_t) this->shmDev.mem;
    const uintptr_t offset = (uintptr_t) frame->offset + sizeof(FrameBuffer);

    fi->dataSize = dataSize;
    fi->fd       = ivshmemGetDMABuf(&this->shmDev, pos + offset, dataSize);
    fi->texture  = NULL;
    if (fi->fd < 0)
    {
      puts("Failed to get the DMA buffer for the frame");
      return NULL;
    }
  }

  return fi;
}
#endif

/* chromaticity coordinates are transported in SMPTE ST 2086 units of
 * 0.00002 */
#define LG_ST2086_UNIT 0.00002

static bool mat3Inverse(const double m[9], double out[9])
{
  const double det =
    m[0] * (m[4] * m[8] - m[5] * m[7]) -
    m[1] * (m[3] * m[8] - m[5] * m[6]) +
    m[2] * (m[3] * m[7] - m[4] * m[6]);

  if (det == 0.0)
    return false;

  const double inv = 1.0 / det;
  out[0] = (m[4] * m[8] - m[5] * m[7]) * inv;
  out[1] = (m[2] * m[7] - m[1] * m[8]) * inv;
  out[2] = (m[1] * m[5] - m[2] * m[4]) * inv;
  out[3] = (m[5] * m[6] - m[3] * m[8]) * inv;
  out[4] = (m[0] * m[8] - m[2] * m[6]) * inv;
  out[5] = (m[2] * m[3] - m[0] * m[5]) * inv;
  out[6] = (m[3] * m[7] - m[4] * m[6]) * inv;
  out[7] = (m[1] * m[6] - m[0] * m[7]) * inv;
  out[8] = (m[0] * m[4] - m[1] * m[3]) * inv;
  return true;
}

static void mat3Mul(const double a[9], const double b[9], double out[9])
{
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      out[r * 3 + c] =
        a[r * 3 + 0] * b[0 * 3 + c] +
        a[r * 3 + 1] * b[1 * 3 + c] +
        a[r * 3 + 2] * b[2 * 3 + c];
}

/* build the linear RGB -> CIE XYZ matrix for a set of primaries and white
 * point (Bruce Lindbloom's method) */
static bool primariesToXYZ(double xr, double yr, double xg, double yg,
    double xb, double yb, double xw, double yw, double out[9])
{
  if (yr == 0.0 || yg == 0.0 || yb == 0.0 || yw == 0.0)
    return false;

  const double Xr = xr / yr, Yr = 1.0, Zr = (1.0 - xr - yr) / yr;
  const double Xg = xg / yg, Yg = 1.0, Zg = (1.0 - xg - yg) / yg;
  const double Xb = xb / yb, Yb = 1.0, Zb = (1.0 - xb - yb) / yb;

  const double P[9] =
  {
    Xr, Xg, Xb,
    Yr, Yg, Yb,
    Zr, Zg, Zb
  };

  double Pinv[9];
  if (!mat3Inverse(P, Pinv))
    return false;

  const double Xw = xw / yw, Yw = 1.0, Zw = (1.0 - xw - yw) / yw;
  const double Sr = Pinv[0] * Xw + Pinv[1] * Yw + Pinv[2] * Zw;
  const double Sg = Pinv[3] * Xw + Pinv[4] * Yw + Pinv[5] * Zw;
  const double Sb = Pinv[6] * Xw + Pinv[7] * Yw + Pinv[8] * Zw;

  out[0] = Sr * Xr; out[1] = Sg * Xg; out[2] = Sb * Xb;
  out[3] = Sr * Yr; out[4] = Sg * Yg; out[5] = Sb * Yb;
  out[6] = Sr * Zr; out[7] = Sg * Zg; out[8] = Sb * Zb;
  return true;
}

/* compute the linear-light matrix that converts the frame's source display
 * primaries to BT.709. Falls back to identity (assume BT.709) if the metadata
 * is missing or degenerate. */
static void lgComputeColorMatrix(LGPlugin * this, const KVMFRFrame * frame)
{
  static const double bt709[9] =
  {
    1.0, 0.0, 0.0,
    0.0, 1.0, 0.0,
    0.0, 0.0, 1.0
  };
  const double * result = bt709;
  double src[9], dst709[9], dst709inv[9], conv[9];

  const double xr = frame->hdrDisplayPrimary[0][0] * LG_ST2086_UNIT;
  const double yr = frame->hdrDisplayPrimary[0][1] * LG_ST2086_UNIT;
  const double xg = frame->hdrDisplayPrimary[1][0] * LG_ST2086_UNIT;
  const double yg = frame->hdrDisplayPrimary[1][1] * LG_ST2086_UNIT;
  const double xb = frame->hdrDisplayPrimary[2][0] * LG_ST2086_UNIT;
  const double yb = frame->hdrDisplayPrimary[2][1] * LG_ST2086_UNIT;
  const double xw = frame->hdrWhitePoint[0] * LG_ST2086_UNIT;
  const double yw = frame->hdrWhitePoint[1] * LG_ST2086_UNIT;

  /* BT.709 / D65 target */
  if (primariesToXYZ(xr, yr, xg, yg, xb, yb, xw, yw, src) &&
      primariesToXYZ(0.640, 0.330, 0.300, 0.600, 0.150, 0.060,
        0.3127, 0.3290, dst709) &&
      mat3Inverse(dst709, dst709inv))
  {
    mat3Mul(dst709inv, src, conv);
    result = conv;
  }
  else
    printf("HDR metadata missing or invalid, assuming BT.709 primaries\n");

  for (int r = 0; r < 3; ++r)
    vec3_set(&this->colorMatrix[r],
      (float)result[r * 3 + 0],
      (float)result[r * 3 + 1],
      (float)result[r * 3 + 2]);

  this->hdrScale = 10000.0f / LG_SCRGB_REFERENCE_WHITE;
}

static void lgFormatInit(LGPlugin * this, const KVMFRFrame * frame,
    LGMPMessage * msg)
{
  this->formatVer    = frame->formatVer;
  this->screenWidth  = frame->screenWidth;
  this->screenHeight = frame->screenHeight;
  this->dataWidth    = frame->dataWidth;
  this->dataHeight   = frame->dataHeight;
  this->frameWidth   = frame->frameWidth;
  this->frameHeight  = frame->frameHeight;
  this->type         = frame->type;

  this->screenScale.x = this->screenWidth  / this->frameWidth ;
  this->screenScale.y = this->screenHeight / this->frameHeight;

  obs_enter_graphics();
  if (this->texture)
  {
    if (this->unpack && this->dstTexture)
    {
      gs_texture_destroy(this->dstTexture);
      this->dstTexture = NULL;
    }

    if (!this->dmabuf)
      gs_texture_unmap(this->texture);

    gs_texture_destroy(this->texture);
    this->texture = NULL;
  }

  this->dataWidth = frame->dataWidth;
  this->unpack    = false;
  this->hdr       = frame->flags & FRAME_FLAG_HDR;
  this->hdrPQ     = frame->flags & FRAME_FLAG_HDR_PQ;

  if (this->hdr && this->hdrPQ)
    lgComputeColorMatrix(this, frame);

  this->bpp = 4;
  switch(this->type)
  {
    case FRAME_TYPE_BGRA:
      this->format     = GS_BGRA_UNORM;
      this->drmFormat  = DRM_FORMAT_ARGB8888;
#if LIBOBS_API_MAJOR_VER >= 28
      this->colorSpace = GS_CS_SRGB;
#endif
      break;

    case FRAME_TYPE_RGBA:
      this->format     = GS_RGBA_UNORM;
      this->drmFormat  = DRM_FORMAT_ARGB8888;
#if LIBOBS_API_MAJOR_VER >= 28
      this->colorSpace = GS_CS_SRGB;
#endif
      break;

    case FRAME_TYPE_RGBA10:
      this->format     = GS_R10G10B10A2;
      this->drmFormat  = DRM_FORMAT_BGRA1010102;
#if LIBOBS_API_MAJOR_VER >= 28
      this->colorSpace = this->hdr ? GS_CS_709_SCRGB : GS_CS_SRGB;
#endif
      break;

    case FRAME_TYPE_RGB_24:
      this->bpp       = 3;
      this->dataWidth = frame->pitch / 4;
      /* fallthrough */

    case FRAME_TYPE_BGR_32:
      this->format    = GS_BGRA_UNORM;
      this->drmFormat = DRM_FORMAT_ARGB8888;
#if LIBOBS_API_MAJOR_VER >= 28
      this->colorSpace = GS_CS_SRGB;
#endif
      this->unpack     = true;
      break;

    case FRAME_TYPE_RGBA16F:
      this->bpp        = 8;
      this->format     = GS_RGBA16F;
      this->drmFormat  = DRM_FORMAT_ABGR16161616F;
#if LIBOBS_API_MAJOR_VER >= 28
      this->colorSpace = GS_CS_709_SCRGB;
#endif
      break;

    default:
      printf("invalid type %d\n", this->type);
      lgmpClientMessageDone(this->frameQueue);
      os_sem_post(this->frameSem);
      obs_leave_graphics();
      return;
  }

#if LIBOBS_API_MAJOR_VER >= 27
  if (this->dmabuf)
  {
    DMAFrameInfo * fi = dmabufOpenDMAFrameInfo(this, msg, frame,
        frame->frameHeight * frame->pitch);
    if (fi && !fi->texture)
    {
      // create the first texture now so we can test if dmabuf is usable
      fi->texture = gs_texture_create_from_dmabuf(
        this->dataWidth,
        this->dataHeight,
        this->drmFormat,
        this->format,
        1,
        &fi->fd,
        &(uint32_t) { frame->pitch },
        &(uint32_t) { 0 },
        &(uint64_t) { 0 });

      if (!fi->texture)
      {
        puts("Failed to create dmabuf texture");
        this->dmabuf = false;
      }

      this->dmabufTested = true;
    }
  }
#else
  (void)drmFormat;
#endif

  if (!this->dmabuf)
  {
    this->texture = gs_texture_create(
      this->dataWidth,
      this->dataHeight,
      this->format,
      1,
      NULL,
      GS_DYNAMIC);

    if (!this->texture)
    {
      printf("create texture failed\n");
      lgmpClientMessageDone(this->frameQueue);
      os_sem_post(this->frameSem);
      obs_leave_graphics();
      return;
    }

    gs_texture_map(this->texture, &this->texData, &this->linesize);
  }

  if (this->unpack)
  {
    // create the render target for format unpacking
    this->dstTexture = gs_texture_create(
      this->frameWidth,
      this->frameHeight,
      GS_BGRA,
      1,
      NULL,
      GS_RENDER_TARGET);
  }

  obs_leave_graphics();
}

static void lgVideoTick(void * data, float seconds)
{
  LGPlugin * this = (LGPlugin *)data;

  if (this->state == STATE_RESTARTING)
  {
    waitThreads(this);

    this->state = STATE_STARTING;
    createThreads(this);
  }
  if (this->state != STATE_RUNNING)
    return;

  LGMP_STATUS status;
  LGMPMessage msg;

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

  const KVMFRFrame * frame = (KVMFRFrame *)msg.mem;

  bool textureValid = (this->dmabufTested && this->dmabuf) || this->texture;
  if (!textureValid || this->formatVer != frame->formatVer)
    lgFormatInit(this, frame, &msg);

#if LIBOBS_API_MAJOR_VER >= 27
  if (this->dmabuf)
  {
    DMAFrameInfo * fi = dmabufOpenDMAFrameInfo(this, &msg, frame,
        frame->frameHeight * frame->pitch);

    if (!fi->texture)
    {
      obs_enter_graphics();
      fi->texture = gs_texture_create_from_dmabuf(
        this->dataWidth,
        this->dataHeight,
        this->drmFormat,
        this->format,
        1,
        &fi->fd,
        &(uint32_t) { frame->pitch },
        &(uint32_t) { 0 },
        &(uint64_t) { 0 });
      obs_leave_graphics();
    }

    lgmpClientMessageDone(this->frameQueue);

    // wait for the frame to be complete before we try to use it
    FrameBuffer * fb = (FrameBuffer *)(((uint8_t*)frame) + frame->offset);
    framebuffer_wait(fb, frame->frameHeight * frame->pitch);

    this->dmaTexture = fi->texture;
    os_sem_post(this->frameSem);
    return;
  }
#endif

  if (!this->texture)
  {
    lgmpClientMessageDone(this->frameQueue);
    os_sem_post(this->frameSem);
    return;
  }

  FrameBuffer * fb = (FrameBuffer *)(((uint8_t*)frame) + frame->offset);
  framebuffer_read(
      fb,
      this->texData   , // dst
      this->linesize  , // dstpitch
      this->dataHeight, // height
      this->dataWidth , // width
      this->bpp       , // bpp
      frame->pitch
  );

  lgmpClientMessageDone(this->frameQueue);
  os_sem_post(this->frameSem);

  obs_enter_graphics();
  gs_texture_unmap(this->texture);
  gs_texture_map(this->texture, &this->texData, &this->linesize);
  obs_leave_graphics();
}

static void lgVideoRender(void * data, gs_effect_t * effect)
{
  LGPlugin * this = (LGPlugin *)data;
  gs_texture_t * texture;

#if LIBOBS_API_MAJOR_VER >= 27
  texture = this->dmaTexture;
  if (!texture)
    texture = this->texture;
#endif

  if (!texture)
    return;

  const bool hdrDecode =
    this->type == FRAME_TYPE_RGBA10 && this->hdr && this->hdrPQ;

  if (this->type == FRAME_TYPE_RGB_24 || this->type == FRAME_TYPE_BGR_32)
  {
    effect = this->unpackEffect;
    gs_effect_set_texture(this->image, texture);
    struct vec2 outputSize;
    vec2_set(&outputSize, this->frameWidth, this->frameHeight);
    gs_effect_set_vec2(this->outputSize, &outputSize);
    gs_effect_set_int(this->swap, this->type == FRAME_TYPE_RGB_24 ? 1 : 0);
  }
  else if (hdrDecode)
  {
    /* decode the PQ (ST.2084) encoded frame to linear scRGB and remap the
     * source primaries to BT.709 so OBS receives valid GS_CS_709_SCRGB data */
    effect = this->hdrEffect;
    gs_effect_set_texture(this->hdrImage, texture);
    struct vec2 outputSize;
    vec2_set(&outputSize, this->frameWidth, this->frameHeight);
    gs_effect_set_vec2(this->hdrOutputSize, &outputSize);
    for (int i = 0; i < 3; ++i)
      gs_effect_set_vec3(this->hdrColorMatrix[i], &this->colorMatrix[i]);
    gs_effect_set_float(this->hdrScaleParam, this->hdrScale);
  }
  else
  {
    effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);
    gs_eparam_t * image = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture(image, texture);
  }

  if (this->unpack)
    texture = this->dstTexture;

  while (gs_effect_loop(effect, "Draw"))
    gs_draw_sprite(texture, 0, 0, 0);

  if (this->cursorVisible && this->cursorTex)
  {
    struct matrix4 m4;
    gs_matrix_get(&m4);
    struct gs_rect r =
    {
      .x  = m4.t.x,
      .y  = m4.t.y,
      .cx = (double)this->frameWidth  * m4.x.x,
      .cy = (double)this->frameHeight * m4.y.y
    };
    gs_set_scissor_rect(&r);

    effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_eparam_t * image = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture(image, this->cursorTex);

    gs_matrix_push();
    gs_matrix_translate3f(
        this->cursorRect.x / this->screenScale.x,
        this->cursorRect.y / this->screenScale.y,
        0.0f);

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

#if LIBOBS_API_MAJOR_VER >= 28
static enum gs_color_space lgVideoGetColorSpace(void *data, size_t count,
  const enum gs_color_space *preferred_spaces)
{
  LGPlugin * this = (LGPlugin *)data;
  return this->colorSpace;
}
#endif

static uint32_t lgGetWidth(void * data)
{
  LGPlugin * this = (LGPlugin *)data;
  return this->frameWidth;
}

static uint32_t lgGetHeight(void * data)
{
  LGPlugin * this = (LGPlugin *)data;
  return this->frameHeight;
}

struct obs_source_info lg_source =
{
  .id                    = "looking-glass-obs",
  .type                  = OBS_SOURCE_TYPE_INPUT,
  .output_flags          = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
                           OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_SRGB,
  .get_name              = lgGetName,
  .create                = lgCreate,
  .destroy               = lgDestroy,
  .update                = lgUpdate,
  .get_defaults          = lgGetDefaults,
  .get_properties        = lgGetProperties,
  .video_tick            = lgVideoTick,
  .video_render          = lgVideoRender,
#if LIBOBS_API_MAJOR_VER >= 28
  .video_get_color_space = lgVideoGetColorSpace,
#endif
  .get_width             = lgGetWidth,
  .get_height            = lgGetHeight,
  .icon_type             = OBS_ICON_TYPE_DESKTOP_CAPTURE
};
