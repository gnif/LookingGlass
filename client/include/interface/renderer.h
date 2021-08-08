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

#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "app.h"
#include "common/KVMFR.h"
#include "common/framebuffer.h"

#define IS_LG_RENDERER_VALID(x) \
  ((x)->get_name       && \
   (x)->create         && \
   (x)->initialize     && \
   (x)->deinitialize   && \
   (x)->on_restart     && \
   (x)->on_resize      && \
   (x)->on_mouse_shape && \
   (x)->on_mouse_event && \
   (x)->render_startup && \
   (x)->needs_render   && \
   (x)->render)

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
  FrameType         type;    // frame type
  unsigned int      width;   // image width
  unsigned int      height;  // image height
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

typedef struct LG_RendererOps
{
  /* returns the friendly name of the renderer */
  const char * (*get_name)(void);

  /* called pre-creation to allow the renderer to register any options it may
   * have */
  void (*setup)(void);

  /* creates an instance of the renderer
   * Context: lg_run */
  bool (*create)(void ** opaque, const LG_RendererParams params,
      bool * needsOpenGL);

  /* initializes the renderer for use
   * Context: lg_run */
  bool (*initialize)(void * opaque);

  /* deinitializes & frees the renderer
   * Context: lg_run & renderThread */
  void (*deinitialize)(void * opaque);

  /* returns true if the specified feature is supported
   * Context: renderThread */
  bool (*supports)(void * opaque, LG_RendererSupport support);

  /* called when the renderer is to reset it's state
   * Context: lg_run & frameThread */
  void (*on_restart)(void * opaque);

  /* called when the viewport has been resized
   * Context: renderThrtead */
  void (*on_resize)(void * opaque, const int width, const int height,
      const double scale, const LG_RendererRect destRect,
      LG_RendererRotate rotate);

  /* called when the mouse shape has changed
   * Context: cursorThread */
  bool (*on_mouse_shape)(void * opaque, const LG_RendererCursor cursor,
      const int width, const int height, const int pitch,
      const uint8_t * data);

  /* called when the mouse has moved or changed visibillity
   * Context: cursorThread */
  bool (*on_mouse_event)(void * opaque, const bool visible,
      const int x, const int y);

  /* called when the frame format has changed
   * Context: frameThread */
  bool (*on_frame_format)(void * opaque, const LG_RendererFormat format);

  /* called when there is a new frame
   * Context: frameThread */
  bool (*on_frame)(void * opaque, const FrameBuffer * frame, int dmaFD,
      const FrameDamageRect * damage, int damageCount);

  /* called when the rederer is to startup
   * Context: renderThread */
  bool (*render_startup)(void * opaque, bool useDMA);

  /* returns if the render method must be called even if nothing has changed.
   * Context: renderThread */
  bool (*needs_render)(void * opaque);

  /* called to render the scene
   * Context: renderThread */
  bool (*render)(void * opaque, LG_RendererRotate rotate, const bool newFrame,
      const bool invalidateWindow, void (*preSwap)(void * udata), void * udata);
}
LG_RendererOps;
