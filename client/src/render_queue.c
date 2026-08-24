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

#include "render_queue.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/debug.h"
#include "common/locking.h"
#include "main.h"

typedef struct RenderCommand
{
  struct RenderCommand * next;

  RenderQueueSource source;
  uint64_t          generation;
  uint64_t          cursorGeneration;
  uint64_t          transitionSerial;

  enum
  {
    SW_SURFACE_OP_CONFIGURE_TRANSITION,
    SW_SURFACE_OP_UPDATE,
    SURFACE_OP_FORMAT,
    CURSOR_OP_STATE,
    CURSOR_OP_IMAGE,
    CURSOR_OP_COLOR_TRANSFORM,
    CURSOR_OP_WHITE_LEVEL,
    CURSOR_OP_CLEAR,
    SOURCE_OP_TRANSITION,
  }
  op;

  union
  {
    struct
    {
      int width, height;
      uint64_t epoch;
    }
    swSurfaceConfigureTransition;

    struct
    {
      uint64_t epoch;
    }
    swSurfaceUpdate;

    struct
    {
      LG_RendererFormat format;
      bool              rendererSupportsNativeHDR;
    }
    surfaceFormat;

    struct
    {
      bool visible;
      int  x;
      int  y;
      int  hx;
      int  hy;
    }
    cursorState;

    struct
    {
      LG_RendererCursor type;
      int               width;
      int               height;
      int               pitch;
      uint8_t         * data;
    }
    cursorImage;

    struct
    {
      LGColorTransform * data;
    }
    cursorColorTransform;

    struct
    {
      uint32_t value;
    }
    cursorWhiteLevel;

    struct
    {
      bool swSurface;
    }
    sourceTransition;
  };
}
RenderCommand;

typedef enum RenderQueueInvalidate
{
  RENDER_QUEUE_INVALIDATE_NONE,
  RENDER_QUEUE_INVALIDATE_PARTIAL,
  RENDER_QUEUE_INVALIDATE_FULL,
}
RenderQueueInvalidate;

static RenderCommand        * l_renderQueueHead;
static RenderCommand        * l_renderQueueTail;
static LG_Lock                l_renderQueueLock;
static bool                   l_renderQueueInitialized;
static RenderQueueInvalidate l_renderQueueInvalidate;

typedef struct RenderQueueSwSurface
{
  LG_Lock lock;

  /* Transport bitmap storage is borrowed, so callbacks compose it into this
   * persistent image before returning. The renderer consumes it synchronously
   * while lock is held. */
  uint64_t generation;
  uint64_t epoch;
  int      width;
  int      height;
  int      pitch;
  uint8_t * data;

  bool            damageFull;
  int             damageCount;
  FrameDamageRect damage[LG_MAX_FRAME_DAMAGE_RECTS];

  bool          updateQueued;
  RenderCommand updateCommand;
}
RenderQueueSwSurface;

static RenderQueueSwSurface l_swSurface[RENDER_QUEUE_SOURCE_COUNT];
static RenderQueueSource    l_swSurfaceResidentSource;
static uint64_t             l_swSurfaceResidentGeneration;
static uint64_t             l_swSurfaceResidentEpoch;

static RenderQueueSwSurface * swSurfaceFromUpdateCommand(
    const RenderCommand * cmd)
{
  for (int i = 0; i < RENDER_QUEUE_SOURCE_COUNT; ++i)
    if (cmd == &l_swSurface[i].updateCommand)
      return &l_swSurface[i];

  return NULL;
}

static bool              l_showSwSurface;
static bool              l_surfaceFormatValid;
static bool              l_rendererSupportsNativeHDR;
static LG_RendererFormat l_surfaceFormat;

static _Atomic(uint64_t) l_sourceGeneration[RENDER_QUEUE_SOURCE_COUNT];
static _Atomic(uint64_t) l_cursorGeneration[RENDER_QUEUE_SOURCE_COUNT];
static _Atomic(uint64_t) l_transitionSerial;
static LG_Lock           l_sourceLock;
static LG_Lock           l_transitionLock;

static RenderQueueSource l_appliedSource;
static uint64_t          l_appliedGeneration;
static bool              l_appliedSwSurface;

static RenderQueueSourcePrepareFn l_sourcePrepareFn;
static RenderQueueSourceRejectFn  l_sourceRejectFn;
static RenderQueueSourceAppliedFn l_sourceAppliedFn;
static void                     * l_sourceCallbackOpaque;

typedef struct RenderQueueTransition
{
  RenderQueueSource source;
  uint64_t          generation;
  uint64_t          serial;
  bool              swSurface;
  bool              valid;
}
RenderQueueTransition;

static RenderQueueTransition l_pendingTransition;

typedef struct RenderQueueCursor
{
  uint64_t stateGeneration;
  bool     stateValid;
  bool     visible;
  int      x;
  int      y;
  int      hx;
  int      hy;

  uint64_t          imageGeneration;
  bool              imageValid;
  LG_RendererCursor type;
  int               width;
  int               height;
  int               pitch;
  uint8_t         * data;

  uint64_t         colorGeneration;
  bool             colorValid;
  LGColorTransform colorTransform;

  uint64_t whiteGeneration;
  bool     whiteValid;
  uint32_t sdrWhiteLevel;
}
RenderQueueCursor;

typedef struct RenderQueueFormat
{
  uint64_t          generation;
  bool              valid;
  LG_RendererFormat format;
  bool              rendererSupportsNativeHDR;
}
RenderQueueFormat;

static RenderQueueCursor l_cursor[RENDER_QUEUE_SOURCE_COUNT];
static RenderQueueFormat l_format[RENDER_QUEUE_SOURCE_COUNT];
static const LGColorTransform l_identityColorTransform;

static bool sourceValid(RenderQueueSource source)
{
  return source > RENDER_QUEUE_SOURCE_NONE &&
    source < RENDER_QUEUE_SOURCE_COUNT;
}

static bool generationValid(RenderQueueSource source, uint64_t generation)
{
  return sourceValid(source) && generation &&
    atomic_load_explicit(&l_sourceGeneration[source],
        memory_order_acquire) == generation;
}

static bool transitionCommand(const RenderCommand * cmd)
{
  return cmd->op == SOURCE_OP_TRANSITION ||
    cmd->op == SW_SURFACE_OP_CONFIGURE_TRANSITION;
}

static bool cursorCommand(const RenderCommand * cmd)
{
  return cmd->op == CURSOR_OP_STATE ||
    cmd->op == CURSOR_OP_IMAGE ||
    cmd->op == CURSOR_OP_COLOR_TRANSFORM ||
    cmd->op == CURSOR_OP_WHITE_LEVEL ||
    cmd->op == CURSOR_OP_CLEAR;
}

static bool commandValid(const RenderCommand * cmd)
{
  if (transitionCommand(cmd) &&
      atomic_load_explicit(&l_transitionSerial, memory_order_acquire) !=
        cmd->transitionSerial)
    return false;

  if (cmd->source == RENDER_QUEUE_SOURCE_NONE)
    return cmd->op == SOURCE_OP_TRANSITION;
  if (!sourceValid(cmd->source))
    return false;

  const bool cursorCurrent = !cursorCommand(cmd) || atomic_load_explicit(
      &l_cursorGeneration[cmd->source], memory_order_acquire) ==
        cmd->cursorGeneration;
  if (cmd->op == CURSOR_OP_CLEAR)
    return cursorCurrent;
  return generationValid(cmd->source, cmd->generation) && cursorCurrent;
}

static uint64_t swSurfaceDamageArea(const FrameDamageRect * rect)
{
  return (uint64_t)rect->width * rect->height;
}

static FrameDamageRect swSurfaceDamageUnion(
    const FrameDamageRect * a, const FrameDamageRect * b)
{
  const uint32_t left   = min(a->x, b->x);
  const uint32_t top    = min(a->y, b->y);
  const uint32_t right  = max(a->x + a->width, b->x + b->width);
  const uint32_t bottom = max(a->y + a->height, b->y + b->height);

  return (FrameDamageRect)
  {
    .x      = left,
    .y      = top,
    .width  = right  - left,
    .height = bottom - top,
  };
}

static bool swSurfaceDamageTouches(
    const FrameDamageRect * a, const FrameDamageRect * b)
{
  return a->x <= b->x + b->width  && b->x <= a->x + a->width &&
         a->y <= b->y + b->height && b->y <= a->y + a->height;
}

static void swSurfaceDamageReset(RenderQueueSwSurface * surface)
{
  surface->damageFull  = false;
  surface->damageCount = 0;
}

static bool swSurfaceDamagePending(const RenderQueueSwSurface * surface)
{
  return surface->damageFull || surface->damageCount;
}

static void swSurfaceDamageAdd(RenderQueueSwSurface * surface,
    const FrameDamageRect * damage)
{
  if (surface->damageFull)
    return;

  if (damage->x == 0 && damage->y == 0 &&
      damage->width  == (uint32_t)surface->width &&
      damage->height == (uint32_t)surface->height)
  {
    surface->damageFull  = true;
    surface->damageCount = 0;
    return;
  }

  FrameDamageRect rect = *damage;
  for (;;)
  {
    for (int i = 0; i < surface->damageCount;)
    {
      if (!swSurfaceDamageTouches(&rect, &surface->damage[i]))
      {
        ++i;
        continue;
      }

      rect = swSurfaceDamageUnion(&rect, &surface->damage[i]);
      surface->damage[i] = surface->damage[--surface->damageCount];
      i = 0;
    }

    if (surface->damageCount < LG_MAX_FRAME_DAMAGE_RECTS)
      break;

    int      best     = 0;
    uint64_t bestCost = UINT64_MAX;
    for (int i = 0; i < surface->damageCount; ++i)
    {
      const FrameDamageRect merged =
        swSurfaceDamageUnion(&rect, &surface->damage[i]);
      const uint64_t cost = swSurfaceDamageArea(&merged) -
        swSurfaceDamageArea(&surface->damage[i]);
      if (cost < bestCost)
      {
        best     = i;
        bestCost = cost;
      }
    }

    rect = swSurfaceDamageUnion(&rect, &surface->damage[best]);
    surface->damage[best] = surface->damage[--surface->damageCount];
  }

  surface->damage[surface->damageCount++] = rect;

  uint64_t area = 0;
  for (int i = 0; i < surface->damageCount; ++i)
    area += swSurfaceDamageArea(&surface->damage[i]);

  const uint64_t fullArea =
    (uint64_t)surface->width * surface->height;
  if (area >= fullArea - fullArea / 4)
  {
    surface->damageFull  = true;
    surface->damageCount = 0;
  }
}

static void freeCommand(RenderCommand * cmd)
{
  if (cmd->op == SW_SURFACE_OP_UPDATE)
    return;

  switch (cmd->op)
  {
    case CURSOR_OP_IMAGE:
      free(cmd->cursorImage.data);
      break;

    case CURSOR_OP_COLOR_TRANSFORM:
      free(cmd->cursorColorTransform.data);
      break;

    default:
      break;
  }

  free(cmd);
}

static bool queueCommand(RenderCommand * cmd,
    RenderQueueInvalidate invalidate)
{
  bool wake = false;
  cmd->next = NULL;

  LG_LOCK(l_renderQueueLock);
  if (l_renderQueueTail)
    l_renderQueueTail->next = cmd;
  else
    l_renderQueueHead = cmd;
  l_renderQueueTail = cmd;

  if (invalidate > l_renderQueueInvalidate)
  {
    l_renderQueueInvalidate = invalidate;
    wake = true;
  }
  LG_UNLOCK(l_renderQueueLock);
  return wake;
}

/* The surface lock must be held while its embedded command is queued. */
static bool queueSwSurfaceUpdate(RenderQueueSource source,
    RenderQueueSwSurface * surface)
{
  if (surface->updateQueued || !surface->data ||
      !swSurfaceDamagePending(surface))
    return false;

  RenderCommand * cmd = &surface->updateCommand;
  cmd->source                = source;
  cmd->generation            = surface->generation;
  cmd->op                    = SW_SURFACE_OP_UPDATE;
  cmd->swSurfaceUpdate.epoch = surface->epoch;
  surface->updateQueued      = true;
  return queueCommand(cmd, RENDER_QUEUE_INVALIDATE_PARTIAL);
}

static void wakeQueue(bool wake)
{
  if (wake)
    app_invalidateWindow(false);
}

static void enqueueCommand(RenderCommand * cmd,
    RenderQueueInvalidate invalidate)
{
  wakeQueue(queueCommand(cmd, invalidate));
}

static RenderCommand * detachCommands(RenderQueueInvalidate * invalidate)
{
  LG_LOCK(l_renderQueueLock);
  RenderCommand * head    = l_renderQueueHead;
  if (invalidate)
    *invalidate = l_renderQueueInvalidate;
  l_renderQueueHead       = NULL;
  l_renderQueueTail       = NULL;
  l_renderQueueInvalidate = RENDER_QUEUE_INVALIDATE_NONE;
  LG_UNLOCK(l_renderQueueLock);
  return head;
}

static void setCommandSource(RenderCommand * cmd, RenderQueueSource source,
    uint64_t generation)
{
  cmd->source     = source;
  cmd->generation = generation;
  cmd->cursorGeneration = sourceValid(source) ? atomic_load_explicit(
      &l_cursorGeneration[source], memory_order_acquire) : 0;
}

static bool copyCursorImage(RenderCommand * cmd, const void * data)
{
  size_t size;
  if (!data || !lg_rendererCursorValidate(cmd->cursorImage.type,
        cmd->cursorImage.width, cmd->cursorImage.height,
        cmd->cursorImage.pitch, &size))
    return false;

  cmd->cursorImage.data = malloc(size);
  if (!cmd->cursorImage.data)
    return false;

  memcpy(cmd->cursorImage.data, data, size);
  return true;
}

static void updateSurfaceFormat(void)
{
  if (!g_state.ds->setHDRImageDesc)
    return;

  LG_RendererFormat format = {};
  if (l_surfaceFormatValid)
    format = l_surfaceFormat;

  if (l_showSwSurface)
  {
    // The software surface is always 8-bit SDR, regardless of the last frame
    // format received from the active frame provider.
    format.hdr         = false;
    format.hdrPQ       = false;
    format.hdrMetadata = false;
    g_state.ds->setHDRImageDesc(&format);
    atomic_store(&g_state.hdrDescFailed, false);
    return;
  }

  if (!l_surfaceFormatValid)
  {
    g_state.ds->setHDRImageDesc(&format);
    atomic_store(&g_state.hdrDescFailed, false);
    return;
  }

  if (format.hdr && !l_rendererSupportsNativeHDR)
  {
    format.hdr         = false;
    format.hdrPQ       = false;
    format.hdrMetadata = false;
    g_state.ds->setHDRImageDesc(&format);
    DEBUG_WARN("Renderer surface cannot represent the native HDR encoding; "
        "using software HDR mapping");
    atomic_store(&g_state.hdrDescFailed, true);
  }
  else if (!g_state.ds->setHDRImageDesc(&format))
  {
    DEBUG_WARN("Display server failed to apply HDR image description");
    atomic_store(&g_state.hdrDescFailed, true);
  }
  else
    atomic_store(&g_state.hdrDescFailed, false);
}

void renderQueue_init(void)
{
  l_renderQueueHead           = NULL;
  l_renderQueueTail           = NULL;
  l_renderQueueInvalidate     = RENDER_QUEUE_INVALIDATE_NONE;
  l_showSwSurface             = false;
  l_surfaceFormatValid        = false;
  l_rendererSupportsNativeHDR = false;
  l_appliedSource             = RENDER_QUEUE_SOURCE_NONE;
  l_appliedGeneration         = 0;
  l_appliedSwSurface          = false;

  l_swSurfaceResidentSource     = RENDER_QUEUE_SOURCE_NONE;
  l_swSurfaceResidentGeneration = 0;
  l_swSurfaceResidentEpoch      = 0;

  l_sourcePrepareFn           = NULL;
  l_sourceRejectFn            = NULL;
  l_sourceAppliedFn           = NULL;
  l_sourceCallbackOpaque      = NULL;
  l_pendingTransition         = (RenderQueueTransition) {};
  memset(&l_surfaceFormat, 0, sizeof(l_surfaceFormat));
  memset(l_cursor, 0, sizeof(l_cursor));
  memset(l_format, 0, sizeof(l_format));
  memset(l_swSurface, 0, sizeof(l_swSurface));

  for (int i = 0; i < RENDER_QUEUE_SOURCE_COUNT; ++i)
  {
    atomic_store_explicit(&l_sourceGeneration[i], 0, memory_order_relaxed);
    atomic_store_explicit(&l_cursorGeneration[i], 1, memory_order_relaxed);
    LG_LOCK_INIT(l_swSurface[i].lock);
  }
  atomic_store_explicit(&l_transitionSerial, 0, memory_order_relaxed);
  LG_LOCK_INIT(l_renderQueueLock);
  LG_LOCK_INIT(l_sourceLock);
  LG_LOCK_INIT(l_transitionLock);
  l_renderQueueInitialized = true;
}

void renderQueue_free(void)
{
  if (!l_renderQueueInitialized)
    return;

  renderQueue_clear();

  for (int i = 0; i < RENDER_QUEUE_SOURCE_COUNT; ++i)
  {
    free(l_cursor[i].data);
    l_cursor[i].data = NULL;
    free(l_swSurface[i].data);
    l_swSurface[i].data = NULL;
    LG_LOCK_FREE(l_swSurface[i].lock);
  }

  l_renderQueueInitialized = false;
  LG_LOCK_FREE(l_renderQueueLock);
  LG_LOCK_FREE(l_sourceLock);
  LG_LOCK_FREE(l_transitionLock);
}

void renderQueue_clear(void)
{
  RenderCommand * cmd = detachCommands(NULL);
  while(cmd)
  {
    RenderCommand * next = cmd->next;
    RenderQueueSwSurface * surface = swSurfaceFromUpdateCommand(cmd);
    if (surface)
    {
      LG_LOCK(surface->lock);
      surface->updateQueued = false;
      LG_UNLOCK(surface->lock);
    }
    else
      freeCommand(cmd);
    cmd = next;
  }
}

void renderQueue_setSourceFns(RenderQueueSourcePrepareFn prepare,
    RenderQueueSourceRejectFn reject,
    RenderQueueSourceAppliedFn applied, void * opaque)
{
  l_sourcePrepareFn      = prepare;
  l_sourceRejectFn       = reject;
  l_sourceAppliedFn      = applied;
  l_sourceCallbackOpaque = opaque;
}

uint64_t renderQueue_sourceBegin(RenderQueueSource source)
{
  if (!sourceValid(source))
    return 0;

  LG_LOCK(l_sourceLock);
  LG_LOCK(l_swSurface[source].lock);
  const uint64_t previous   = atomic_load_explicit(
      &l_sourceGeneration[source], memory_order_relaxed);
  const uint64_t generation = previous + 1;
  atomic_store_explicit(
      &l_sourceGeneration[source], generation, memory_order_release);

  RenderQueueCursor * cursor = &l_cursor[source];
  if (cursor->stateValid && cursor->stateGeneration == previous)
    cursor->stateGeneration = generation;
  if (cursor->imageValid && cursor->imageGeneration == previous)
    cursor->imageGeneration = generation;
  if (cursor->colorValid && cursor->colorGeneration == previous)
    cursor->colorGeneration = generation;
  if (cursor->whiteValid && cursor->whiteGeneration == previous)
    cursor->whiteGeneration = generation;
  LG_UNLOCK(l_swSurface[source].lock);
  LG_UNLOCK(l_sourceLock);
  return generation;
}

void renderQueue_sourceInvalidate(RenderQueueSource source,
    uint64_t generation)
{
  if (!sourceValid(source) || !generation)
    return;
  LG_LOCK(l_sourceLock);
  LG_LOCK(l_swSurface[source].lock);
  const uint64_t invalidGeneration = generation + 1;
  if (atomic_compare_exchange_strong_explicit(&l_sourceGeneration[source],
        &generation, invalidGeneration, memory_order_acq_rel,
        memory_order_relaxed))
  {
    RenderQueueCursor * cursor = &l_cursor[source];
    if (cursor->stateValid && cursor->stateGeneration == generation)
      cursor->stateGeneration = invalidGeneration;
    if (cursor->imageValid && cursor->imageGeneration == generation)
      cursor->imageGeneration = invalidGeneration;
    if (cursor->colorValid && cursor->colorGeneration == generation)
      cursor->colorGeneration = invalidGeneration;
    if (cursor->whiteValid && cursor->whiteGeneration == generation)
      cursor->whiteGeneration = invalidGeneration;
  }
  LG_UNLOCK(l_swSurface[source].lock);
  LG_UNLOCK(l_sourceLock);
}

void renderQueue_sourceClearCursor(RenderQueueSource source)
{
  if (!sourceValid(source))
    return;

  LG_LOCK(l_sourceLock);
  uint64_t cursorGeneration = atomic_load_explicit(
      &l_cursorGeneration[source], memory_order_relaxed) + 1;
  if (!cursorGeneration)
    ++cursorGeneration;
  atomic_store_explicit(&l_cursorGeneration[source], cursorGeneration,
      memory_order_release);
  free(l_cursor[source].data);
  memset(&l_cursor[source], 0, sizeof(l_cursor[source]));
  const uint64_t sourceGeneration = atomic_load_explicit(
      &l_sourceGeneration[source], memory_order_acquire);
  RenderCommand * cmd = malloc(sizeof(*cmd));
  bool wake = false;
  if (cmd)
  {
    setCommandSource(cmd, source, sourceGeneration);
    cmd->op = CURSOR_OP_CLEAR;
    wake = queueCommand(cmd, RENDER_QUEUE_INVALIDATE_PARTIAL);
  }
  LG_UNLOCK(l_sourceLock);
  wakeQueue(wake);
}

static bool configureSwSurface(RenderQueueSource source,
    uint64_t generation, int width, int height, uint64_t * epoch)
{
  if (width <= 0 || height <= 0 || width > INT_MAX / 4)
  {
    DEBUG_ERROR("Invalid software surface dimensions: %dx%d", width, height);
    return false;
  }

  const int pitch = width * 4;
  if ((size_t)height > SIZE_MAX / (size_t)pitch)
  {
    DEBUG_ERROR("Software surface size overflows: %dx%d", width, height);
    return false;
  }

  RenderQueueSwSurface * surface = &l_swSurface[source];
  LG_LOCK(surface->lock);
  if (generationValid(source, generation) && surface->data &&
      surface->generation == generation &&
      surface->width == width && surface->height == height)
  {
    *epoch = surface->epoch;
    LG_UNLOCK(surface->lock);
    return true;
  }
  LG_UNLOCK(surface->lock);

  uint8_t * data = calloc((size_t)height, (size_t)pitch);
  if (!data)
  {
    DEBUG_ERROR("Failed to allocate software surface: %dx%d", width, height);
    return false;
  }

  LG_LOCK(surface->lock);
  if (!generationValid(source, generation))
  {
    LG_UNLOCK(surface->lock);
    free(data);
    return false;
  }

  if (surface->data && surface->generation == generation &&
      surface->width == width && surface->height == height)
  {
    *epoch = surface->epoch;
    LG_UNLOCK(surface->lock);
    free(data);
    return true;
  }

  uint64_t nextEpoch = surface->epoch + 1;
  if (!nextEpoch)
    ++nextEpoch;

  free(surface->data);
  surface->generation = generation;
  surface->epoch      = nextEpoch;
  surface->width      = width;
  surface->height     = height;
  surface->pitch      = pitch;
  surface->data       = data;
  swSurfaceDamageReset(surface);
  *epoch = nextEpoch;
  LG_UNLOCK(surface->lock);
  return true;
}

bool renderQueue_sourceSwSurfaceConfigure(RenderQueueSource source,
    uint64_t generation, int width, int height)
{
  if (!generationValid(source, generation))
    return false;

  uint64_t epoch;
  return configureSwSurface(source, generation, width, height, &epoch);
}

static bool getSwSurfaceConfiguration(RenderQueueSource source,
    uint64_t generation, int width, int height, uint64_t * epoch)
{
  RenderQueueSwSurface * surface = &l_swSurface[source];
  LG_LOCK(surface->lock);
  const bool valid = generationValid(source, generation) && surface->data &&
    surface->generation == generation &&
    surface->width == width && surface->height == height;
  if (valid)
    *epoch = surface->epoch;
  LG_UNLOCK(surface->lock);
  return valid;
}

static uint64_t enqueueTransition(RenderCommand * cmd,
    atomic_uint_least64_t * publishedSerial)
{
  LG_LOCK(l_transitionLock);
  const uint64_t serial = atomic_load_explicit(
      &l_transitionSerial, memory_order_relaxed) + 1;
  cmd->transitionSerial = serial;
  const bool wake = queueCommand(cmd, RENDER_QUEUE_INVALIDATE_FULL);
  atomic_store_explicit(
      &l_transitionSerial, serial, memory_order_release);
  if (publishedSerial)
    atomic_store_explicit(publishedSerial, serial, memory_order_release);
  LG_UNLOCK(l_transitionLock);
  wakeQueue(wake);
  return serial;
}

uint64_t renderQueue_sourceTransition(RenderQueueSource source,
    uint64_t generation, bool swSurface,
    atomic_uint_least64_t * publishedSerial)
{
  if (source != RENDER_QUEUE_SOURCE_NONE &&
      !generationValid(source, generation))
    return 0;

  if (source == RENDER_QUEUE_SOURCE_NONE)
  {
    generation = 0;
    swSurface  = false;
  }

  RenderCommand * cmd = malloc(sizeof(*cmd));
  if (!cmd)
    return 0;

  setCommandSource(cmd, source, generation);
  cmd->op                         = SOURCE_OP_TRANSITION;
  cmd->sourceTransition.swSurface = swSurface;
  return enqueueTransition(cmd, publishedSerial);
}

uint64_t renderQueue_sourceSwSurfaceConfigureTransition(
    RenderQueueSource source, uint64_t generation, int width, int height,
    atomic_uint_least64_t * publishedSerial)
{
  if (!generationValid(source, generation))
    return 0;

  RenderCommand * cmd = malloc(sizeof(*cmd));
  if (!cmd)
    return 0;

  uint64_t epoch;
  if (!getSwSurfaceConfiguration(
        source, generation, width, height, &epoch))
  {
    free(cmd);
    return 0;
  }

  setCommandSource(cmd, source, generation);
  cmd->op                                  =
    SW_SURFACE_OP_CONFIGURE_TRANSITION;
  cmd->swSurfaceConfigureTransition.width  = width;
  cmd->swSurfaceConfigureTransition.height = height;
  cmd->swSurfaceConfigureTransition.epoch  = epoch;
  return enqueueTransition(cmd, publishedSerial);
}

static bool clipSwSurfaceRect(const RenderQueueSwSurface * surface,
    int * x, int * y, int * width, int * height,
    int * sourceX, int * sourceY)
{
  const int64_t left   = max((int64_t)*x, 0);
  const int64_t top    = max((int64_t)*y, 0);
  const int64_t right  = min((int64_t)*x + *width,
      (int64_t)surface->width);
  const int64_t bottom = min((int64_t)*y + *height,
      (int64_t)surface->height);
  if (right <= left || bottom <= top)
    return false;

  if (sourceX)
    *sourceX = (int)(left - *x);
  if (sourceY)
    *sourceY = (int)(top - *y);
  *x      = (int)left;
  *y      = (int)top;
  *width  = (int)(right  - left);
  *height = (int)(bottom - top);
  return true;
}

void renderQueue_sourceSwSurfaceDrawFill(RenderQueueSource source,
    uint64_t generation, int x, int y, int width, int height,
    uint32_t color)
{
  if (!generationValid(source, generation) || width <= 0 || height <= 0)
    return;

  RenderQueueSwSurface * surface = &l_swSurface[source];
  LG_LOCK(surface->lock);
  if (surface->generation != generation ||
      !generationValid(source, generation) ||
      !clipSwSurfaceRect(surface,
        &x, &y, &width, &height, NULL, NULL))
  {
    LG_UNLOCK(surface->lock);
    return;
  }

  uint8_t * row = surface->data +
    (size_t)y * surface->pitch + (size_t)x * 4;
  for (int dy = 0; dy < height; ++dy)
  {
    uint32_t * dst = (uint32_t *)row;
    for (int dx = 0; dx < width; ++dx)
      dst[dx] = color;
    row += surface->pitch;
  }

  const FrameDamageRect damage =
  {
    .x      = (uint32_t)x,
    .y      = (uint32_t)y,
    .width  = (uint32_t)width,
    .height = (uint32_t)height,
  };
  swSurfaceDamageAdd(surface, &damage);
  const bool wake = queueSwSurfaceUpdate(source, surface);
  LG_UNLOCK(surface->lock);
  wakeQueue(wake);
}

void renderQueue_sourceSwSurfaceDrawBitmap(RenderQueueSource source,
    uint64_t generation, int x, int y, int width, int height, int stride,
    const void * data, bool topDown)
{
  if (!generationValid(source, generation))
    return;

  if (width <= 0 || height <= 0 || stride <= 0)
  {
    if (width < 0 || height < 0 || stride < 0)
      DEBUG_ERROR("Invalid software surface bitmap dimensions: "
          "%dx%d, stride: %d",
          width, height, stride);
    return;
  }

  if (!data)
  {
    DEBUG_ERROR("Software surface bitmap data is NULL");
    return;
  }

  if (width > stride / 4)
  {
    DEBUG_ERROR("Software surface bitmap stride is too small: "
        "width: %d, stride: %d", width, stride);
    return;
  }

  if ((size_t)height > SIZE_MAX / (size_t)stride)
  {
    DEBUG_ERROR("Software surface bitmap size overflows: "
        "height: %d, stride: %d", height, stride);
    return;
  }

  RenderQueueSwSurface * surface = &l_swSurface[source];
  LG_LOCK(surface->lock);
  int sourceX;
  int sourceY;
  const int sourceHeight = height;
  if (surface->generation != generation ||
      !generationValid(source, generation) ||
      !clipSwSurfaceRect(surface,
        &x, &y, &width, &height, &sourceX, &sourceY))
  {
    LG_UNLOCK(surface->lock);
    return;
  }

  const size_t copySize = (size_t)width * 4;
  uint8_t * dst = surface->data +
    (size_t)y * surface->pitch + (size_t)x * 4;
  for (int dy = 0; dy < height; ++dy)
  {
    const int sourceRow = topDown ?
      sourceY + dy : sourceHeight - sourceY - dy - 1;
    const uint8_t * src = (const uint8_t *)data +
      (size_t)sourceRow * stride + (size_t)sourceX * 4;
    memcpy(dst, src, copySize);
    dst += surface->pitch;
  }

  const FrameDamageRect damage =
  {
    .x      = (uint32_t)x,
    .y      = (uint32_t)y,
    .width  = (uint32_t)width,
    .height = (uint32_t)height,
  };
  swSurfaceDamageAdd(surface, &damage);
  const bool wake = queueSwSurfaceUpdate(source, surface);
  LG_UNLOCK(surface->lock);
  wakeQueue(wake);
}

void renderQueue_sourceSurfaceFormat(RenderQueueSource source,
    uint64_t generation, const LG_RendererFormat format,
    bool rendererSupportsNativeHDR)
{
  if (!generationValid(source, generation))
    return;

  RenderCommand * cmd = malloc(sizeof(*cmd));
  if (!cmd)
    return;

  setCommandSource(cmd, source, generation);
  cmd->op                                      = SURFACE_OP_FORMAT;
  cmd->surfaceFormat.format                    = format;
  cmd->surfaceFormat.rendererSupportsNativeHDR =
    rendererSupportsNativeHDR;
  enqueueCommand(cmd, RENDER_QUEUE_INVALIDATE_FULL);
}

void renderQueue_sourceCursorState(RenderQueueSource source,
    uint64_t generation, bool visible, int x, int y, int hx, int hy)
{
  if (!generationValid(source, generation))
    return;

  RenderCommand * cmd = malloc(sizeof(*cmd));
  if (!cmd)
    return;

  setCommandSource(cmd, source, generation);
  cmd->op                  = CURSOR_OP_STATE;
  cmd->cursorState.visible = visible;
  cmd->cursorState.x       = x;
  cmd->cursorState.y       = y;
  cmd->cursorState.hx      = hx;
  cmd->cursorState.hy      = hy;
  enqueueCommand(cmd, RENDER_QUEUE_INVALIDATE_NONE);
}

void renderQueue_sourceCursorImage(RenderQueueSource source,
    uint64_t generation, LG_RendererCursor type,
    int width, int height, int pitch, const void * data)
{
  if (!generationValid(source, generation))
    return;

  RenderCommand * cmd = malloc(sizeof(*cmd));
  if (!cmd)
    return;

  setCommandSource(cmd, source, generation);
  cmd->op                 = CURSOR_OP_IMAGE;
  cmd->cursorImage.type   = type;
  cmd->cursorImage.width  = width;
  cmd->cursorImage.height = height;
  cmd->cursorImage.pitch  = pitch;
  cmd->cursorImage.data   = NULL;
  if (!copyCursorImage(cmd, data))
    freeCommand(cmd);
  else
    enqueueCommand(cmd, RENDER_QUEUE_INVALIDATE_NONE);
}

void renderQueue_sourceCursorColorTransform(RenderQueueSource source,
    uint64_t generation, const LGColorTransform * transform)
{
  if (!generationValid(source, generation) || !transform)
    return;

  RenderCommand * cmd = malloc(sizeof(*cmd));
  if (!cmd)
    return;

  LGColorTransform * copy = malloc(sizeof(*copy));
  if (!copy)
  {
    free(cmd);
    return;
  }
  *copy = *transform;

  setCommandSource(cmd, source, generation);
  cmd->op                        = CURSOR_OP_COLOR_TRANSFORM;
  cmd->cursorColorTransform.data = copy;
  enqueueCommand(cmd, RENDER_QUEUE_INVALIDATE_NONE);
}

void renderQueue_sourceCursorWhiteLevel(RenderQueueSource source,
    uint64_t generation, uint32_t sdrWhiteLevel)
{
  if (!generationValid(source, generation))
    return;

  RenderCommand * cmd = malloc(sizeof(*cmd));
  if (!cmd)
    return;

  setCommandSource(cmd, source, generation);
  cmd->op                     = CURSOR_OP_WHITE_LEVEL;
  cmd->cursorWhiteLevel.value = sdrWhiteLevel;
  enqueueCommand(cmd, RENDER_QUEUE_INVALIDATE_NONE);
}

static bool swSurfaceCommandMatches(const RenderQueueSwSurface * surface,
    const RenderCommand * cmd, uint64_t epoch)
{
  return surface->data && surface->generation == cmd->generation &&
    surface->epoch == epoch;
}

static void uploadSwSurfaceFull(RenderQueueSource source,
    RenderQueueSwSurface * surface)
{
  RENDERER(swSurfaceConfigure, surface->width, surface->height);
  RENDERER(swSurfaceDrawBitmap,
      0, 0, surface->width, surface->height,
      surface->pitch, surface->data, true);
  swSurfaceDamageReset(surface);
  l_swSurfaceResidentSource     = source;
  l_swSurfaceResidentGeneration = surface->generation;
  l_swSurfaceResidentEpoch      = surface->epoch;
}

static bool swSurfaceResident(RenderQueueSource source,
    const RenderQueueSwSurface * surface)
{
  return l_swSurfaceResidentSource == source &&
    l_swSurfaceResidentGeneration == surface->generation &&
    l_swSurfaceResidentEpoch == surface->epoch;
}

static void uploadSwSurfaceDamage(RenderQueueSource source,
    RenderQueueSwSurface * surface)
{
  if (!swSurfaceResident(source, surface))
  {
    uploadSwSurfaceFull(source, surface);
    return;
  }

  if (surface->damageFull)
    RENDERER(swSurfaceDrawBitmap,
        0, 0, surface->width, surface->height,
        surface->pitch, surface->data, true);
  else
    for (int i = 0; i < surface->damageCount; ++i)
    {
      const FrameDamageRect * damage = &surface->damage[i];
      uint8_t * data = surface->data +
        (size_t)damage->y * surface->pitch + (size_t)damage->x * 4;
      RENDERER(swSurfaceDrawBitmap,
          damage->x, damage->y, damage->width, damage->height,
          surface->pitch, data, true);
    }

  swSurfaceDamageReset(surface);
}

static bool swSurfaceTransitionMatches(
    const RenderQueueSwSurface * surface, const RenderCommand * cmd,
    bool configure)
{
  const uint64_t epoch = configure ?
    cmd->swSurfaceConfigureTransition.epoch : surface->epoch;
  return swSurfaceCommandMatches(surface, cmd, epoch) &&
    (!configure ||
      (surface->width  == cmd->swSurfaceConfigureTransition.width &&
       surface->height == cmd->swSurfaceConfigureTransition.height));
}

static bool prepareSwSurfaceTransition(const RenderCommand * cmd,
    bool configure)
{
  RenderQueueSwSurface * surface = &l_swSurface[cmd->source];
  LG_LOCK(surface->lock);

  const bool valid = swSurfaceTransitionMatches(surface, cmd, configure);
  if (valid)
  {
    if (configure || !swSurfaceResident(cmd->source, surface))
      uploadSwSurfaceFull(cmd->source, surface);
    else
      uploadSwSurfaceDamage(cmd->source, surface);
  }

  LG_UNLOCK(surface->lock);
  return valid;
}

static bool swSurfaceTransitionReady(const RenderCommand * cmd,
    bool configure)
{
  RenderQueueSwSurface * surface = &l_swSurface[cmd->source];
  LG_LOCK(surface->lock);
  const bool ready = swSurfaceTransitionMatches(surface, cmd, configure);
  LG_UNLOCK(surface->lock);
  return ready;
}

/* Called with l_sourceLock held. The marker may be recycled into the live
 * queue after an older generation was detached from it. */
static bool processSwSurfaceUpdate(RenderCommand * cmd, bool commandValid)
{
  RenderQueueSwSurface * surface = &l_swSurface[cmd->source];
  LG_LOCK(surface->lock);

  if (!surface->updateQueued)
  {
    LG_UNLOCK(surface->lock);
    return false;
  }

  surface->updateQueued = false;
  const bool matches = commandValid &&
    swSurfaceCommandMatches(surface, cmd, cmd->swSurfaceUpdate.epoch);
  const bool active = l_appliedSwSurface &&
    l_appliedSource == cmd->source &&
    l_appliedGeneration == cmd->generation;
  if (matches && active)
    uploadSwSurfaceDamage(cmd->source, surface);

  bool wake = false;
  if (swSurfaceDamagePending(surface) &&
      l_appliedSwSurface && l_appliedSource == cmd->source &&
      l_appliedGeneration == surface->generation &&
      generationValid(cmd->source, surface->generation))
    wake = queueSwSurfaceUpdate(cmd->source, surface);

  LG_UNLOCK(surface->lock);
  return wake;
}

static void applyCursor(RenderQueueSource source, uint64_t generation)
{
  RenderQueueCursor * cursor = &l_cursor[source];

  if (cursor->imageValid && cursor->imageGeneration == generation)
    RENDERER(onMouseShape, cursor->type, cursor->width, cursor->height,
        cursor->pitch, cursor->data);

  if (cursor->imageValid && cursor->imageGeneration == generation &&
      cursor->stateValid && cursor->stateGeneration == generation)
    RENDERER(onMouseEvent, cursor->visible, cursor->x, cursor->y,
        cursor->hx, cursor->hy);
  else
    RENDERER(onMouseEvent, false, 0, 0, 0, 0);

  if (g_state.lgr->ops.onMouseColorTransform)
    g_state.lgr->ops.onMouseColorTransform(g_state.lgr,
        cursor->colorValid && cursor->colorGeneration == generation ?
          &cursor->colorTransform : &l_identityColorTransform);

  if (g_state.lgr->ops.onMouseWhiteLevel)
    g_state.lgr->ops.onMouseWhiteLevel(g_state.lgr,
        cursor->whiteValid && cursor->whiteGeneration == generation ?
          cursor->sdrWhiteLevel : LG_SDR_WHITE_LEVEL_DEFAULT);
}

static void applySourceTransition(const RenderCommand * cmd,
    bool showSwSurface)
{
  if (l_showSwSurface != showSwSurface)
  {
    RENDERER(swSurfaceShow, showSwSurface);
    l_showSwSurface = showSwSurface;
  }

  l_surfaceFormatValid        = false;
  l_rendererSupportsNativeHDR = false;
  if (sourceValid(cmd->source))
  {
    const RenderQueueFormat * format = &l_format[cmd->source];
    if (format->valid && format->generation == cmd->generation)
    {
      l_surfaceFormat                   = format->format;
      l_surfaceFormatValid              = true;
      l_rendererSupportsNativeHDR       =
        format->rendererSupportsNativeHDR;
    }
  }
  updateSurfaceFormat();

  l_appliedSource     = cmd->source;
  l_appliedGeneration = cmd->generation;
  l_appliedSwSurface  = showSwSurface;

  if (sourceValid(cmd->source))
    applyCursor(cmd->source, cmd->generation);
  else
    RENDERER(onMouseEvent, false, 0, 0, 0, 0);

  l_pendingTransition.source     = cmd->source;
  l_pendingTransition.generation = cmd->generation;
  l_pendingTransition.serial     = cmd->transitionSerial;
  l_pendingTransition.swSurface  = l_appliedSwSurface;
  l_pendingTransition.valid      = true;
}

static bool prepareSourceTransition(const RenderCommand * cmd)
{
  return !l_sourcePrepareFn || l_sourcePrepareFn(l_sourceCallbackOpaque,
      cmd->source, cmd->generation, cmd->transitionSerial);
}

static void rejectSourceTransition(const RenderCommand * cmd)
{
  if (l_sourceRejectFn)
    l_sourceRejectFn(l_sourceCallbackOpaque,
        cmd->source, cmd->transitionSerial);
}

bool renderQueue_process(void)
{
  RenderQueueInvalidate invalidate;
  RenderCommand * cmd = detachCommands(&invalidate);
  while(cmd)
  {
    RenderCommand * next = cmd->next;
    const bool transition = transitionCommand(cmd);
    bool wake = false;
    if (transition)
      LG_LOCK(l_transitionLock);
    LG_LOCK(l_sourceLock);

    const bool validCommand = commandValid(cmd);
    if (!validCommand)
    {
      if (cmd->op == SW_SURFACE_OP_UPDATE)
        wake = processSwSurfaceUpdate(cmd, false);
      LG_UNLOCK(l_sourceLock);
      if (transition)
        LG_UNLOCK(l_transitionLock);
      freeCommand(cmd);
      wakeQueue(wake);
      cmd = next;
      continue;
    }

    switch(cmd->op)
    {
      case SW_SURFACE_OP_CONFIGURE_TRANSITION:
        if (!swSurfaceTransitionReady(cmd, true))
        {
          DEBUG_ERROR("Software surface configuration is unavailable");
          rejectSourceTransition(cmd);
          break;
        }
        if (!prepareSourceTransition(cmd))
          break;
        if (!prepareSwSurfaceTransition(cmd, true))
        {
          rejectSourceTransition(cmd);
          break;
        }
        applySourceTransition(cmd, true);
        break;

      case SW_SURFACE_OP_UPDATE:
        wake = processSwSurfaceUpdate(cmd, true);
        break;

      case SURFACE_OP_FORMAT:
      {
        RenderQueueFormat * format = &l_format[cmd->source];
        format->generation                = cmd->generation;
        format->valid                     = true;
        format->format                    = cmd->surfaceFormat.format;
        format->rendererSupportsNativeHDR =
          cmd->surfaceFormat.rendererSupportsNativeHDR;

        if (l_appliedSource == cmd->source &&
            l_appliedGeneration == cmd->generation &&
            !l_appliedSwSurface)
        {
          l_surfaceFormat             = format->format;
          l_surfaceFormatValid        = true;
          l_rendererSupportsNativeHDR =
            format->rendererSupportsNativeHDR;
          updateSurfaceFormat();
        }
        break;
      }

      case CURSOR_OP_STATE:
      {
        RenderQueueCursor * cursor = &l_cursor[cmd->source];
        cursor->stateGeneration = cmd->generation;
        cursor->stateValid      = true;
        cursor->visible         = cmd->cursorState.visible;
        cursor->x               = cmd->cursorState.x;
        cursor->y               = cmd->cursorState.y;
        cursor->hx              = cmd->cursorState.hx;
        cursor->hy              = cmd->cursorState.hy;

        if (l_appliedSource == cmd->source &&
            l_appliedGeneration == cmd->generation)
          RENDERER(onMouseEvent,
              cursor->imageValid &&
                cursor->imageGeneration == cmd->generation &&
                cursor->visible,
              cursor->x, cursor->y, cursor->hx, cursor->hy);
        break;
      }

      case CURSOR_OP_IMAGE:
      {
        RenderQueueCursor * cursor = &l_cursor[cmd->source];
        free(cursor->data);
        cursor->imageGeneration = cmd->generation;
        cursor->imageValid      = true;
        cursor->type            = cmd->cursorImage.type;
        cursor->width           = cmd->cursorImage.width;
        cursor->height          = cmd->cursorImage.height;
        cursor->pitch           = cmd->cursorImage.pitch;
        cursor->data            = cmd->cursorImage.data;
        cmd->cursorImage.data   = NULL;

        if (l_appliedSource == cmd->source &&
            l_appliedGeneration == cmd->generation)
        {
          RENDERER(onMouseShape, cursor->type, cursor->width,
              cursor->height, cursor->pitch, cursor->data);
          if (cursor->stateValid &&
              cursor->stateGeneration == cmd->generation)
            RENDERER(onMouseEvent, cursor->visible, cursor->x, cursor->y,
                cursor->hx, cursor->hy);
        }
        break;
      }

      case CURSOR_OP_COLOR_TRANSFORM:
      {
        RenderQueueCursor * cursor = &l_cursor[cmd->source];
        cursor->colorGeneration = cmd->generation;
        cursor->colorValid      = true;
        cursor->colorTransform  = *cmd->cursorColorTransform.data;

        if (l_appliedSource == cmd->source &&
            l_appliedGeneration == cmd->generation &&
            g_state.lgr->ops.onMouseColorTransform)
          g_state.lgr->ops.onMouseColorTransform(g_state.lgr,
              &cursor->colorTransform);
        break;
      }

      case CURSOR_OP_WHITE_LEVEL:
      {
        RenderQueueCursor * cursor = &l_cursor[cmd->source];
        cursor->whiteGeneration = cmd->generation;
        cursor->whiteValid      = true;
        cursor->sdrWhiteLevel   = cmd->cursorWhiteLevel.value;

        if (l_appliedSource == cmd->source &&
            l_appliedGeneration == cmd->generation &&
            g_state.lgr->ops.onMouseWhiteLevel)
          g_state.lgr->ops.onMouseWhiteLevel(g_state.lgr,
              cursor->sdrWhiteLevel);
        break;
      }

      case CURSOR_OP_CLEAR:
        if (l_appliedSource == cmd->source)
        {
          RENDERER(onMouseEvent, false, 0, 0, 0, 0);
          if (g_state.lgr->ops.onMouseColorTransform)
            g_state.lgr->ops.onMouseColorTransform(
                g_state.lgr, &l_identityColorTransform);
          if (g_state.lgr->ops.onMouseWhiteLevel)
            g_state.lgr->ops.onMouseWhiteLevel(
                g_state.lgr, LG_SDR_WHITE_LEVEL_DEFAULT);
        }
        break;

      case SOURCE_OP_TRANSITION:
      {
        const bool swSurface =
          cmd->source != RENDER_QUEUE_SOURCE_NONE &&
          cmd->sourceTransition.swSurface;
        if (swSurface && !swSurfaceTransitionReady(cmd, false))
        {
          DEBUG_ERROR("Software surface source is unavailable");
          rejectSourceTransition(cmd);
          break;
        }
        if (!prepareSourceTransition(cmd))
          break;
        if (swSurface && !prepareSwSurfaceTransition(cmd, false))
        {
          rejectSourceTransition(cmd);
          break;
        }
        applySourceTransition(cmd, swSurface);
        break;
      }
    }
    LG_UNLOCK(l_sourceLock);
    if (transition)
      LG_UNLOCK(l_transitionLock);
    freeCommand(cmd);
    wakeQueue(wake);
    cmd = next;
  }

  return invalidate == RENDER_QUEUE_INVALIDATE_FULL;
}

void renderQueue_presented(void)
{
  if (!l_pendingTransition.valid)
    return;

  const RenderQueueTransition pending = l_pendingTransition;
  l_pendingTransition.valid = false;

  if (l_sourceAppliedFn)
    l_sourceAppliedFn(l_sourceCallbackOpaque, pending.source,
        pending.generation, pending.serial, pending.swSurface);
}
