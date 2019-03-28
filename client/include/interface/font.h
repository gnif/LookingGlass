/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
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

typedef void * LG_FontObj;
typedef struct LG_FontBitmap
{
  void * reserved;

  unsigned int width, height;
  unsigned int bpp; // bytes per pixel
  uint8_t      * pixels;
}
LG_FontBitmap;

typedef bool            (* LG_FontCreate      )(LG_FontObj * opaque, const char * font_name, unsigned int size);
typedef void            (* LG_FontDestroy     )(LG_FontObj opaque);
typedef LG_FontBitmap * (* LG_FontRender      )(LG_FontObj opaque, unsigned int fg_color, const char * text);
typedef void            (* LG_FontRelease     )(LG_FontObj opaque, LG_FontBitmap * bitmap);

typedef struct LG_Font
{
  // mandatory support
  const char *        name;
  LG_FontCreate       create;
  LG_FontDestroy      destroy;
  LG_FontRender       render;
  LG_FontRelease      release;
}
LG_Font;