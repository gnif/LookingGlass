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

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "common/debug.h"
#include "common/ll.h"
#include "main.h"

struct ll * l_renderQueue = NULL;
static bool              l_showSwSurface;
static bool              l_surfaceFormatValid;
static bool              l_rendererSupportsNativeHDR;
static LG_RendererFormat l_surfaceFormat;

static _Atomic(uint64_t) l_sourceGeneration[RENDER_QUEUE_SOURCE_COUNT];
static _Atomic(uint64_t) l_transitionSerial;
static LG_Lock           l_sourceLock;
static LG_Lock           l_transitionLock;

static RenderQueueSource l_appliedSource;
static uint64_t          l_appliedGeneration;
static bool              l_appliedSwSurface;

static RenderQueueSourcePrepareFn l_sourcePrepareFn;
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

static bool commandValid(const RenderCommand * cmd)
{
  if (transitionCommand(cmd) &&
      atomic_load_explicit(&l_transitionSerial, memory_order_acquire) !=
        cmd->transitionSerial)
    return false;

  if (cmd->source == RENDER_QUEUE_SOURCE_NONE)
    return cmd->op == SOURCE_OP_TRANSITION;

  return generationValid(cmd->source, cmd->generation);
}

static void freeCommand(RenderCommand * cmd)
{
  switch (cmd->op)
  {
    case SW_SURFACE_OP_DRAW_BITMAP:
      free(cmd->swSurfaceDrawBitmap.data);
      break;

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

static void setCommandSource(RenderCommand * cmd, RenderQueueSource source,
    uint64_t generation)
{
  cmd->source     = source;
  cmd->generation = generation;
}

static bool copyCursorImage(RenderCommand * cmd, const void * data)
{
  if (!data || cmd->cursorImage.height <= 0 || cmd->cursorImage.pitch <= 0)
    return false;

  if ((size_t)cmd->cursorImage.height >
      SIZE_MAX / (size_t)cmd->cursorImage.pitch)
    return false;

  const size_t size =
    (size_t)cmd->cursorImage.height * (size_t)cmd->cursorImage.pitch;
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
  l_renderQueue               = ll_new();
  l_showSwSurface             = false;
  l_surfaceFormatValid        = false;
  l_rendererSupportsNativeHDR = false;
  l_appliedSource             = RENDER_QUEUE_SOURCE_NONE;
  l_appliedGeneration         = 0;
  l_appliedSwSurface          = false;
  l_sourcePrepareFn           = NULL;
  l_sourceAppliedFn           = NULL;
  l_sourceCallbackOpaque      = NULL;
  l_pendingTransition         = (RenderQueueTransition) {};
  memset(&l_surfaceFormat, 0, sizeof(l_surfaceFormat));
  memset(l_cursor, 0, sizeof(l_cursor));
  memset(l_format, 0, sizeof(l_format));

  for (int i = 0; i < RENDER_QUEUE_SOURCE_COUNT; ++i)
    atomic_store_explicit(&l_sourceGeneration[i], 0, memory_order_relaxed);
  atomic_store_explicit(&l_transitionSerial, 0, memory_order_relaxed);
  LG_LOCK_INIT(l_sourceLock);
  LG_LOCK_INIT(l_transitionLock);
}

void renderQueue_free(void)
{
  if (!l_renderQueue)
    return;

  renderQueue_clear();

  for (int i = 0; i < RENDER_QUEUE_SOURCE_COUNT; ++i)
  {
    free(l_cursor[i].data);
    l_cursor[i].data = NULL;
  }

  ll_free(l_renderQueue);
  l_renderQueue = NULL;
  LG_LOCK_FREE(l_sourceLock);
  LG_LOCK_FREE(l_transitionLock);
}

void renderQueue_clear(void)
{
  RenderCommand * cmd;
  while(ll_shift(l_renderQueue, (void **)&cmd))
    freeCommand(cmd);
}

void renderQueue_setSourceFns(RenderQueueSourcePrepareFn prepare,
    RenderQueueSourceAppliedFn applied, void * opaque)
{
  l_sourcePrepareFn      = prepare;
  l_sourceAppliedFn      = applied;
  l_sourceCallbackOpaque = opaque;
}

uint64_t renderQueue_sourceBegin(RenderQueueSource source)
{
  if (!sourceValid(source))
    return 0;

  LG_LOCK(l_sourceLock);
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
  LG_UNLOCK(l_sourceLock);
  return generation;
}

void renderQueue_sourceInvalidate(RenderQueueSource source,
    uint64_t generation)
{
  if (!sourceValid(source) || !generation)
    return;
  LG_LOCK(l_sourceLock);
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
  LG_UNLOCK(l_sourceLock);
}

void renderQueue_sourceClearCursor(RenderQueueSource source)
{
  if (!sourceValid(source))
    return;

  LG_LOCK(l_sourceLock);
  free(l_cursor[source].data);
  memset(&l_cursor[source], 0, sizeof(l_cursor[source]));
  LG_UNLOCK(l_sourceLock);
}

static uint64_t enqueueTransition(RenderCommand * cmd,
    atomic_uint_least64_t * publishedSerial)
{
  LG_LOCK(l_transitionLock);
  const uint64_t serial = atomic_load_explicit(
      &l_transitionSerial, memory_order_relaxed) + 1;
  cmd->transitionSerial = serial;
  if (!ll_push(l_renderQueue, cmd))
  {
    LG_UNLOCK(l_transitionLock);
    free(cmd);
    return 0;
  }
  atomic_store_explicit(
      &l_transitionSerial, serial, memory_order_release);
  if (publishedSerial)
    atomic_store_explicit(publishedSerial, serial, memory_order_release);
  LG_UNLOCK(l_transitionLock);

  app_invalidateWindow(true);
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

  setCommandSource(cmd, source, generation);
  cmd->op                                  =
    SW_SURFACE_OP_CONFIGURE_TRANSITION;
  cmd->swSurfaceConfigureTransition.width  = width;
  cmd->swSurfaceConfigureTransition.height = height;
  return enqueueTransition(cmd, publishedSerial);
}

void renderQueue_sourceSwSurfaceDrawFill(RenderQueueSource source,
    uint64_t generation, int x, int y, int width, int height,
    uint32_t color)
{
  if (!generationValid(source, generation))
    return;

  RenderCommand * cmd = malloc(sizeof(*cmd));
  if (!cmd)
    return;

  setCommandSource(cmd, source, generation);
  cmd->op                       = SW_SURFACE_OP_DRAW_FILL;
  cmd->swSurfaceDrawFill.x      = x;
  cmd->swSurfaceDrawFill.y      = y;
  cmd->swSurfaceDrawFill.width  = width;
  cmd->swSurfaceDrawFill.height = height;
  cmd->swSurfaceDrawFill.color  = color;
  if (!ll_push(l_renderQueue, cmd))
  {
    free(cmd);
    return;
  }

  app_invalidateWindow(true);
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

  if ((size_t)height > SIZE_MAX / (size_t)stride)
  {
    DEBUG_ERROR("Software surface bitmap size overflows: "
        "height: %d, stride: %d",
        height, stride);
    return;
  }

  const size_t size = (size_t)height * (size_t)stride;
  RenderCommand * cmd = malloc(sizeof(*cmd));
  if (!cmd)
  {
    DEBUG_ERROR("Failed to allocate software surface bitmap command");
    return;
  }

  uint8_t * copy = malloc(size);
  if (!copy)
  {
    DEBUG_ERROR("Failed to allocate %zu bytes for software surface bitmap",
        size);
    free(cmd);
    return;
  }

  memcpy(copy, data, size);

  setCommandSource(cmd, source, generation);
  cmd->op                          = SW_SURFACE_OP_DRAW_BITMAP;
  cmd->swSurfaceDrawBitmap.x       = x;
  cmd->swSurfaceDrawBitmap.y       = y;
  cmd->swSurfaceDrawBitmap.width   = width;
  cmd->swSurfaceDrawBitmap.height  = height;
  cmd->swSurfaceDrawBitmap.stride  = stride;
  cmd->swSurfaceDrawBitmap.data    = copy;
  cmd->swSurfaceDrawBitmap.topDown = topDown;

  if (!ll_push(l_renderQueue, cmd))
  {
    freeCommand(cmd);
    return;
  }

  app_invalidateWindow(true);
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
  if (!ll_push(l_renderQueue, cmd))
  {
    free(cmd);
    return;
  }

  app_invalidateWindow(true);
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
  if (!ll_push(l_renderQueue, cmd))
    free(cmd);
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
  if (!copyCursorImage(cmd, data) || !ll_push(l_renderQueue, cmd))
    freeCommand(cmd);
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
  if (!ll_push(l_renderQueue, cmd))
    freeCommand(cmd);
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
  if (!ll_push(l_renderQueue, cmd))
    free(cmd);
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

void renderQueue_process(void)
{
  RenderCommand * cmd;
  while(ll_shift(l_renderQueue, (void **)&cmd))
  {
    const bool transition = transitionCommand(cmd);
    if (transition)
      LG_LOCK(l_transitionLock);
    LG_LOCK(l_sourceLock);

    if (!commandValid(cmd))
    {
      LG_UNLOCK(l_sourceLock);
      if (transition)
        LG_UNLOCK(l_transitionLock);
      freeCommand(cmd);
      continue;
    }

    switch(cmd->op)
    {
      case SW_SURFACE_OP_CONFIGURE_TRANSITION:
        if (!prepareSourceTransition(cmd))
          break;
        RENDERER(swSurfaceConfigure,
            cmd->swSurfaceConfigureTransition.width,
            cmd->swSurfaceConfigureTransition.height);
        RENDERER(swSurfaceDrawFill, 0, 0,
            cmd->swSurfaceConfigureTransition.width,
            cmd->swSurfaceConfigureTransition.height, 0);
        applySourceTransition(cmd, true);
        break;

      case SW_SURFACE_OP_DRAW_FILL:
        RENDERER(swSurfaceDrawFill,
            cmd->swSurfaceDrawFill.x    , cmd->swSurfaceDrawFill.y,
            cmd->swSurfaceDrawFill.width, cmd->swSurfaceDrawFill.height,
            cmd->swSurfaceDrawFill.color);
        break;

      case SW_SURFACE_OP_DRAW_BITMAP:
        RENDERER(swSurfaceDrawBitmap,
            cmd->swSurfaceDrawBitmap.x     , cmd->swSurfaceDrawBitmap.y,
            cmd->swSurfaceDrawBitmap.width , cmd->swSurfaceDrawBitmap.height,
            cmd->swSurfaceDrawBitmap.stride, cmd->swSurfaceDrawBitmap.data,
            cmd->swSurfaceDrawBitmap.topDown);
        free(cmd->swSurfaceDrawBitmap.data);
        cmd->swSurfaceDrawBitmap.data = NULL;
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

      case SOURCE_OP_TRANSITION:
        if (!prepareSourceTransition(cmd))
          break;
        applySourceTransition(cmd,
            cmd->source != RENDER_QUEUE_SOURCE_NONE &&
              cmd->sourceTransition.swSurface);
        break;
    }
    LG_UNLOCK(l_sourceLock);
    if (transition)
      LG_UNLOCK(l_transitionLock);
    freeCommand(cmd);
  }
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
