/**
 * Looking Glass
 * Copyright © 2017-2024 The Looking Glass Authors
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

#include "interface/renderer.h"

static const char * vulkan_getName(void)
{
  return "Vulkan";
}

static void vulkan_setup(void)
{
}

static bool vulkan_create(LG_Renderer ** renderer, const LG_RendererParams params,
    bool * needsOpenGL)
{
  DEBUG_FATAL("vulkan_create not implemented");
}

static bool vulkan_initialize(LG_Renderer * renderer)
{
  DEBUG_FATAL("vulkan_initialize not implemented");
}

static void vulkan_deinitialize(LG_Renderer * renderer)
{
  DEBUG_FATAL("vulkan_deinitialize not implemented");
}

static bool vulkan_supports(LG_Renderer * renderer, LG_RendererSupport flag)
{
  DEBUG_FATAL("vulkan_supports not implemented");
}

static void vulkan_onRestart(LG_Renderer * renderer)
{
}

static void vulkan_onResize(LG_Renderer * renderer, const int width, const int height, const double scale,
    const LG_RendererRect destRect, LG_RendererRotate rotate)
{
  DEBUG_FATAL("vulkan_onResize not implemented");
}

static bool vulkan_onMouseShape(LG_Renderer * renderer, const LG_RendererCursor cursor,
    const int width, const int height,
    const int pitch, const uint8_t * data)
{
  DEBUG_FATAL("vulkan_onMouseShape not implemented");
}

static bool vulkan_onMouseEvent(LG_Renderer * renderer, const bool visible,
    int x, int y, const int hx, const int hy)
{
  DEBUG_FATAL("vulkan_onMouseEvent not implemented");
}

static bool vulkan_onFrameFormat(LG_Renderer * renderer, const LG_RendererFormat format)
{
  DEBUG_FATAL("vulkan_onFrameFormat not implemented");
}

static bool vulkan_onFrame(LG_Renderer * renderer, const FrameBuffer * frame, int dmaFd,
    const FrameDamageRect * damageRects, int damageRectsCount)
{
  DEBUG_FATAL("vulkan_onFrame not implemented");
}

static bool vulkan_renderStartup(LG_Renderer * renderer, bool useDMA)
{
  DEBUG_FATAL("vulkan_renderStartup not implemented");
}

static bool vulkan_render(LG_Renderer * renderer, LG_RendererRotate rotate,
    const bool newFrame, const bool invalidateWindow,
    void (*preSwap)(void * udata), void * udata)
{
  DEBUG_FATAL("vulkan_render not implemented");
}

static void * vulkan_createTexture(LG_Renderer * renderer,
  int width, int height, uint8_t * data)
{
  DEBUG_FATAL("vulkan_createTexture not implemented");
}

static void vulkan_freeTexture(LG_Renderer * renderer, void * texture)
{
  DEBUG_FATAL("vulkan_freeTexture not implemented");
}

static void vulkan_spiceConfigure(LG_Renderer * renderer, int width, int height)
{
  DEBUG_FATAL("vulkan_spiceConfigure not implemented");
}

static void vulkan_spiceDrawFill(LG_Renderer * renderer, int x, int y, int width,
    int height, uint32_t color)
{
  DEBUG_FATAL("vulkan_spiceDrawFill not implemented");
}

static void vulkan_spiceDrawBitmap(LG_Renderer * renderer, int x, int y, int width,
    int height, int stride, uint8_t * data, bool topDown)
{
  DEBUG_FATAL("vulkan_spiceDrawBitmap not implemented");
}

static void vulkan_spiceShow(LG_Renderer * renderer, bool show)
{
  DEBUG_FATAL("vulkan_spiceShow not implemented");
}

struct LG_RendererOps LGR_Vulkan =
{
  .getName       = vulkan_getName,
  .setup         = vulkan_setup,
  .create        = vulkan_create,
  .initialize    = vulkan_initialize,
  .deinitialize  = vulkan_deinitialize,
  .supports      = vulkan_supports,
  .onRestart     = vulkan_onRestart,
  .onResize      = vulkan_onResize,
  .onMouseShape  = vulkan_onMouseShape,
  .onMouseEvent  = vulkan_onMouseEvent,
  .onFrameFormat = vulkan_onFrameFormat,
  .onFrame       = vulkan_onFrame,
  .renderStartup = vulkan_renderStartup,
  .render        = vulkan_render,
  .createTexture = vulkan_createTexture,
  .freeTexture   = vulkan_freeTexture,

  .spiceConfigure  = vulkan_spiceConfigure,
  .spiceDrawFill   = vulkan_spiceDrawFill,
  .spiceDrawBitmap = vulkan_spiceDrawBitmap,
  .spiceShow       = vulkan_spiceShow
};
