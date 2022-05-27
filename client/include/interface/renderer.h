/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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

#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "app.h"
#include "common/KVMFR.h"
#include "common/framebuffer.h"

#define IS_LG_RENDERER_VALID(x) \
  ((x)->getName         && \
   (x)->create          && \
   (x)->initialize      && \
   (x)->deinitialize    && \
   (x)->onRestart       && \
   (x)->onResize        && \
   (x)->onMouseShape    && \
   (x)->onMouseEvent    && \
   (x)->renderStartup   && \
   (x)->render          && \
   (x)->createTexture   && \
   (x)->freeTexture     && \
   (x)->spiceConfigure  && \
   (x)->spiceDrawFill   && \
   (x)->spiceDrawBitmap && \
   (x)->spiceShow)

typedef struct LG_RendererParams
{
  bool quickSplash;
}
LG_RendererParams;

typedef enum LG_RendererSupport
{
  LG_SUPPORTS_DMABUF
}
LG_RendererSupport;

typedef enum LG_RendererRotate
{
  LG_ROTATE_0,
  LG_ROTATE_90,
  LG_ROTATE_180,
  LG_ROTATE_270
}
LG_RendererRotate;

// kept out of the enum so gcc doesn't warn when it's missing from a switch
// statement.
#define LG_ROTATE_MAX (LG_ROTATE_270+1)

typedef struct LG_RendererFormat
{
  FrameType         type;         // frame type
  unsigned int      screenWidth;  // actual width of the host
  unsigned int      screenHeight; // actual height of the host
  unsigned int      frameWidth;   // width of frame transmitted
  unsigned int      frameHeight;  // height of frame transmitted
  unsigned int      stride;  // scanline width (zero if compresed)
  unsigned int      pitch;   // scanline bytes (or compressed size)
  unsigned int      bpp;     // bits per pixel (zero if compressed)
  LG_RendererRotate rotate;  // guest rotation
}
LG_RendererFormat;

typedef struct LG_RendererRect
{
  bool valid;
  int  x;
  int  y;
  int  w;
  int  h;
}
LG_RendererRect;

typedef enum LG_RendererCursor
{
  LG_CURSOR_COLOR       ,
  LG_CURSOR_MONOCHROME  ,
  LG_CURSOR_MASKED_COLOR
}
LG_RendererCursor;

typedef struct LG_Renderer LG_Renderer;

typedef struct LG_RendererOps
{
  /* returns the friendly name of the renderer */
  const char * (*getName)(void);

  /* called pre-creation to allow the renderer to register any options it may
   * have */
  void (*setup)(void);

  /* creates an instance of the renderer
   * Context: lg_run */
  bool (*create)(LG_Renderer ** renderer, const LG_RendererParams params,
      bool * needsOpenGL);

  /* initializes the renderer for use
   * Context: lg_run */
  bool (*initialize)(LG_Renderer * renderer);

  /* deinitializes & frees the renderer
   * Context: lg_run & renderThread */
  void (*deinitialize)(LG_Renderer * renderer);

  /* returns true if the specified feature is supported
   * Context: renderThread */
  bool (*supports)(LG_Renderer * renderer, LG_RendererSupport support);

  /* called when the renderer is to reset it's state
   * Context: lg_run & frameThread */
  void (*onRestart)(LG_Renderer * renderer);

  /* called when the viewport has been resized
   * Context: renderThrtead */
  void (*onResize)(LG_Renderer * renderer, const int width, const int height,
      const double scale, const LG_RendererRect destRect,
      LG_RendererRotate rotate);

  /* called when the mouse shape has changed
   * Context: cursorThread */
  bool (*onMouseShape)(LG_Renderer * renderer, const LG_RendererCursor cursor,
      const int width, const int height, const int pitch, const uint8_t * data);

  /* called when the mouse has moved or changed visibillity
   * Context: cursorThread */
  bool (*onMouseEvent)(LG_Renderer * renderer, const bool visible, int x, int y,
      const int hx, const int hy);

  /* called when the frame format has changed
   * Context: frameThread */
  bool (*onFrameFormat)(LG_Renderer * renderer,
      const LG_RendererFormat format);

  /* called when there is a new frame
   * Context: frameThread */
  bool (*onFrame)(LG_Renderer * renderer, const FrameBuffer * frame, int dmaFD,
      const FrameDamageRect * damage, int damageCount);

  /* called when the rederer is to startup
   * Context: renderThread */
  bool (*renderStartup)(LG_Renderer * renderer, bool useDMA);

  /* called to render the scene
   * Context: renderThread */
  bool (*render)(LG_Renderer * renderer, LG_RendererRotate rotate,
      const bool newFrame, const bool invalidateWindow,
      void (*preSwap)(void * udata), void * udata);

  /* called to create a texture from the specified 32-bit RGB image data. This
   * method is for use with Dear ImGui
   * Context: renderThread */
  void * (*createTexture)(LG_Renderer * renderer,
      int width, int height, uint8_t * data);

  /* called to free a texture previously created by createTexture. This method
   * is for use with Dear ImGui
   * Context: renderThread */
  void (*freeTexture)(LG_Renderer * renderer, void * texture);

  /* setup the spice display */
  void (*spiceConfigure)(LG_Renderer * renderer, int width, int height);

  /* draw a filled rect on the spice display with the specified color */
  void (*spiceDrawFill)(LG_Renderer * renderer, int x, int y, int width,
      int height, uint32_t color);

  /* draw an image on the spice display, data is RGBA32 */
  void (*spiceDrawBitmap)(LG_Renderer * renderer, int x, int y, int width,
      int height, int stride, uint8_t * data, bool topDown);

  /* show the spice display */
  void (*spiceShow)(LG_Renderer * renderer, bool show);
}
LG_RendererOps;

typedef struct LG_Renderer
{
  LG_RendererOps ops;
}
LG_Renderer;
