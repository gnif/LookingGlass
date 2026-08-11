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

#include <stdint.h>
#include <string.h>

#include "common/debug.h"
#include "common/ll.h"
#include "main.h"
#include "overlays.h"

struct ll * l_renderQueue = NULL;
static bool l_showSwSurface;
static bool l_surfaceFormatValid;
static bool l_rendererSupportsNativeHDR;
static LG_RendererFormat l_surfaceFormat;

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
    return;

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
  memset(&l_surfaceFormat, 0, sizeof(l_surfaceFormat));
}

void renderQueue_free(void)
{
  if (!l_renderQueue)
    return;

  renderQueue_clear();
  ll_free(l_renderQueue);
}

void renderQueue_clear(void)
{
  RenderCommand * cmd;
  while(ll_shift(l_renderQueue, (void **)&cmd))
  {
    if (cmd->op == SW_SURFACE_OP_DRAW_BITMAP)
      free(cmd->swSurfaceDrawBitmap.data);
    free(cmd);
  }
}

void renderQueue_swSurfaceConfigure(int width, int height)
{
  RenderCommand * cmd = malloc(sizeof(*cmd));
  cmd->op                        = SW_SURFACE_OP_CONFIGURE;
  cmd->swSurfaceConfigure.width  = width;
  cmd->swSurfaceConfigure.height = height;
  ll_push(l_renderQueue, cmd);
  app_invalidateWindow(true);
}

void renderQueue_swSurfaceDrawFill(int x, int y, int width, int height,
    uint32_t color)
{
  RenderCommand * cmd = malloc(sizeof(*cmd));
  cmd->op                       = SW_SURFACE_OP_DRAW_FILL;
  cmd->swSurfaceDrawFill.x      = x;
  cmd->swSurfaceDrawFill.y      = y;
  cmd->swSurfaceDrawFill.width  = width;
  cmd->swSurfaceDrawFill.height = height;
  cmd->swSurfaceDrawFill.color  = color;
  ll_push(l_renderQueue, cmd);
  app_invalidateWindow(true);
}

void renderQueue_swSurfaceDrawBitmap(int x, int y, int width, int height,
    int stride, void * data, bool topDown)
{
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
    free(copy);
    free(cmd);
    return;
  }

  app_invalidateWindow(true);
}

void renderQueue_swSurfaceShow(bool show)
{
  RenderCommand * cmd = malloc(sizeof(*cmd));
  cmd->op                 = SW_SURFACE_OP_SHOW;
  cmd->swSurfaceShow.show = show;
  ll_push(l_renderQueue, cmd);
  app_invalidateWindow(true);
}

void renderQueue_surfaceFormat(const LG_RendererFormat format,
    bool rendererSupportsNativeHDR)
{
  RenderCommand * cmd = malloc(sizeof(*cmd));
  cmd->op = SURFACE_OP_FORMAT;
  cmd->surfaceFormat.format = format;
  cmd->surfaceFormat.rendererSupportsNativeHDR =
    rendererSupportsNativeHDR;
  ll_push(l_renderQueue, cmd);
  app_invalidateWindow(true);
}

void renderQueue_cursorState(bool visible, int x, int y, int hx, int hy)
{
  RenderCommand * cmd      = malloc(sizeof(*cmd));
  cmd->op                  = CURSOR_OP_STATE;
  cmd->cursorState.visible = visible;
  cmd->cursorState.x       = x;
  cmd->cursorState.y       = y;
  cmd->cursorState.hx      = hx;
  cmd->cursorState.hy      = hy;
  ll_push(l_renderQueue, cmd);
}

void renderQueue_cursorImage(LG_RendererCursor type,
    int width, int height, int pitch, uint8_t * data)
{
  RenderCommand * cmd     = malloc(sizeof(*cmd));
  cmd->op                 = CURSOR_OP_IMAGE;
  cmd->cursorImage.type   = type;
  cmd->cursorImage.width  = width;
  cmd->cursorImage.height = height;
  cmd->cursorImage.pitch  = pitch;
  cmd->cursorImage.data   = data;
  ll_push(l_renderQueue, cmd);
}

void renderQueue_process(void)
{
  RenderCommand * cmd;
  while(ll_shift(l_renderQueue, (void **)&cmd))
  {
    switch(cmd->op)
    {
      case SW_SURFACE_OP_CONFIGURE:
        RENDERER(swSurfaceConfigure,
            cmd->swSurfaceConfigure.width,
            cmd->swSurfaceConfigure.height);
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
        break;

      case SW_SURFACE_OP_SHOW:
        l_showSwSurface = cmd->swSurfaceShow.show;
        RENDERER(swSurfaceShow, cmd->swSurfaceShow.show);
        updateSurfaceFormat();
        if (cmd->swSurfaceShow.show)
          overlaySplash_show(false);
        break;

      case SURFACE_OP_FORMAT:
        l_surfaceFormat = cmd->surfaceFormat.format;
        l_surfaceFormatValid = true;
        l_rendererSupportsNativeHDR =
          cmd->surfaceFormat.rendererSupportsNativeHDR;
        updateSurfaceFormat();
        break;

      case CURSOR_OP_STATE:
        RENDERER(onMouseEvent, cmd->cursorState.visible, cmd->cursorState.x,
            cmd->cursorState.y, cmd->cursorState.hx, cmd->cursorState.hy);
        break;

      case CURSOR_OP_IMAGE:
        RENDERER(onMouseShape,
            cmd->cursorImage.type,
            cmd->cursorImage.width, cmd->cursorImage.height,
            cmd->cursorImage.pitch, cmd->cursorImage.data);
        free(cmd->cursorImage.data);
    }
    free(cmd);
  }
}
