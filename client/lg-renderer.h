/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#pragma once
#include <stdint.h>
#include <stdbool.h>

#include <SDL2/SDL.h>
#include <SDL_ttf.h>

#define IS_LG_RENDERER_VALID(x) \
  ((x)->get_name       && \
   (x)->initialize     && \
   (x)->configure      && \
   (x)->deconfigure    && \
   (x)->deinitialize   && \
   (x)->is_compatible  && \
   (x)->on_resize      && \
   (x)->on_mouse_shape && \
   (x)->on_mouse_event && \
   (x)->render)

typedef struct LG_RendererParams
{
  TTF_Font * font;
  bool       showFPS;
  bool       resample;
}
LG_RendererParams;

typedef struct LG_RendererFormat
{
  unsigned int width;   // image width
  unsigned int height;  // image height
  unsigned int stride;  // scanline width
  unsigned int pitch;   // scanline bytes
  unsigned int bpp;     // bits per pixel
}
LG_RendererFormat;

typedef struct LG_RendererRect
{
  int          x;
  int          y;
  unsigned int w;
  unsigned int h;
}
LG_RendererRect;

typedef enum LG_RendererCursor
{
  LG_CURSOR_COLOR       ,
  LG_CURSOR_MONOCHROME  ,
  LG_CURSOR_MASKED_COLOR
}
LG_RendererCursor;

typedef const char * (* LG_RendererGetName       )();
typedef bool         (* LG_RendererInitialize    )(void ** opaque, const LG_RendererParams params, Uint32 * sdlFlags);
typedef bool         (* LG_RendererConfigure     )(void * opaque, SDL_Window *window, const LG_RendererFormat format);
typedef void         (* LG_RendererDeConfigure   )(void * opaque);
typedef void         (* LG_RendererDeInitialize  )(void * opaque);
typedef bool         (* LG_RendererIsCompatible  )(void * opaque, const LG_RendererFormat format);
typedef void         (* LG_RendererOnResize      )(void * opaque, const int width, const int height, const LG_RendererRect destRect);
typedef bool         (* LG_RendererOnMouseShape  )(void * opaque, const LG_RendererCursor cursor, const int width, const int height, const int pitch, const uint8_t * data);
typedef bool         (* LG_RendererOnMouseEvent  )(void * opaque, const bool visible , const int x, const int y);
typedef bool         (* LG_RendererOnFrameEvent  )(void * opaque, const uint8_t * data);
typedef bool         (* LG_RendererRender        )(void * opaque);

typedef struct LG_Renderer
{
  LG_RendererGetName      get_name;
  LG_RendererInitialize   initialize;
  LG_RendererConfigure    configure;
  LG_RendererDeConfigure  deconfigure;
  LG_RendererDeInitialize deinitialize;
  LG_RendererIsCompatible is_compatible;
  LG_RendererOnResize     on_resize;
  LG_RendererOnMouseShape on_mouse_shape;
  LG_RendererOnMouseEvent on_mouse_event;
  LG_RendererOnFrameEvent on_frame_event;
  LG_RendererRender       render;
}
LG_Renderer;