/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>

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
  ((x)->get_name      && \
   (x)->initialize    && \
   (x)->deinitialize  && \
   (x)->is_compatible && \
   (x)->on_resize     && \
   (x)->render)

typedef struct LG_RendererParams
{
  SDL_Window * window;
  TTF_Font   * font;
  bool         vsync;
  bool         showFPS;
  int          width;
  int          height;
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

typedef const char * (* LG_RendererGetName     )();
typedef bool         (* LG_RendererInitialize  )(void ** opaque, const LG_RendererParams params, const LG_RendererFormat format);
typedef void         (* LG_RendererDeInitialize)(void * opaque);
typedef bool         (* LG_RendererIsCompatible)(void * opaque, const LG_RendererFormat format);
typedef void         (* LG_RendererOnResize    )(void * opaque, const int width, const int height);
typedef bool         (* LG_RendererRender      )(void * opaque, const LG_RendererRect destRect, const uint8_t * data, bool resample);

typedef struct LG_Renderer
{
  LG_RendererGetName      get_name;
  LG_RendererInitialize   initialize;
  LG_RendererDeInitialize deinitialize;
  LG_RendererIsCompatible is_compatible;
  LG_RendererOnResize     on_resize;
  LG_RendererRender       render;
}
LG_Renderer;