/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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
   (x)->on_alert       && \
   (x)->on_help        && \
   (x)->on_show_fps    && \
   (x)->render_startup && \
   (x)->render         && \
   (x)->update_fps)

typedef struct LG_RendererParams
{
//  TTF_Font * font;
//  TTF_Font * alertFont;
  bool       quickSplash;
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

// returns the friendly name of the renderer
typedef const char * (* LG_RendererGetName)();

// called pre-creation to allow the renderer to register any options it might have
typedef void         (* LG_RendererSetup)();

typedef bool         (* LG_RendererCreate       )(void ** opaque, const LG_RendererParams params, bool * needsOpenGL);
typedef bool         (* LG_RendererInitialize   )(void * opaque);
typedef void         (* LG_RendererDeInitialize )(void * opaque);
typedef bool         (* LG_RendererSupports     )(void * opaque, LG_RendererSupport support);
typedef void         (* LG_RendererOnRestart    )(void * opaque);
typedef void         (* LG_RendererOnResize     )(void * opaque, const int width, const int height, const double scale, const LG_RendererRect destRect, LG_RendererRotate rotate);
typedef bool         (* LG_RendererOnMouseShape )(void * opaque, const LG_RendererCursor cursor, const int width, const int height, const int pitch, const uint8_t * data);
typedef bool         (* LG_RendererOnMouseEvent )(void * opaque, const bool visible , const int x, const int y);
typedef bool         (* LG_RendererOnFrameFormat)(void * opaque, const LG_RendererFormat format, bool useDMA);
typedef bool         (* LG_RendererOnFrame      )(void * opaque, const FrameBuffer * frame, int dmaFD);
typedef void         (* LG_RendererOnAlert      )(void * opaque, const LG_MsgAlert alert, const char * message, bool ** closeFlag);
typedef void         (* LG_RendererOnHelp       )(void * opaque, const char * message);
typedef void         (* LG_RendererOnShowFPS    )(void * opaque, bool showFPS);
typedef bool         (* LG_RendererRenderStartup)(void * opaque);
typedef bool         (* LG_RendererRender       )(void * opaque, LG_RendererRotate rotate);
typedef void         (* LG_RendererUpdateFPS    )(void * opaque, const float avgUPS, const float avgFPS);

typedef struct LG_Renderer
{
  LG_RendererGetName      get_name;
  LG_RendererSetup        setup;

  LG_RendererCreate         create;
  LG_RendererInitialize     initialize;
  LG_RendererDeInitialize   deinitialize;
  LG_RendererSupports       supports;
  LG_RendererOnRestart      on_restart;
  LG_RendererOnResize       on_resize;
  LG_RendererOnMouseShape   on_mouse_shape;
  LG_RendererOnMouseEvent   on_mouse_event;
  LG_RendererOnFrameFormat  on_frame_format;
  LG_RendererOnFrame        on_frame;
  LG_RendererOnAlert        on_alert;
  LG_RendererOnHelp         on_help;
  LG_RendererOnShowFPS      on_show_fps;
  LG_RendererRenderStartup  render_startup;
  LG_RendererRender         render;
  LG_RendererUpdateFPS      update_fps;
}
LG_Renderer;
