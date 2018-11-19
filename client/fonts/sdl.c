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

#include <stdlib.h>
#include <stdbool.h>

#include "lg-font.h"
#include "debug.h"

#include <SDL2/SDL_ttf.h>
#include <fontconfig/fontconfig.h>

static int        g_initCount  = 0;
static FcConfig * g_fontConfig = NULL;

struct Inst
{
  TTF_Font * font;
};

static bool lgf_sdl_create(LG_FontObj * opaque, const char * font_name, unsigned int size)
{
  if (g_initCount++ == 0)
  {
    if (TTF_Init() < 0)
    {
      DEBUG_ERROR("TTF_Init Failed");
      return false;
    }

    g_fontConfig = FcInitLoadConfigAndFonts();
    if (!g_fontConfig)
    {
      DEBUG_ERROR("FcInitLoadConfigAndFonts Failed");
      return false;
    }
  }

  *opaque = malloc(sizeof(struct Inst));
  if (!*opaque)
  {
    DEBUG_INFO("Failed to allocate %lu bytes", sizeof(struct Inst));
    return false;
  }
  memset(*opaque, 0, sizeof(struct Inst));

  struct Inst * this = (struct Inst *)*opaque;

 if (!font_name)
    font_name = "FreeMono";

  FcPattern * pat = FcNameParse((const FcChar8*)font_name);
  FcConfigSubstitute (g_fontConfig, pat, FcMatchPattern);
  FcDefaultSubstitute(pat);
  FcResult result;
  FcChar8 * file = NULL;
  FcPattern * font = FcFontMatch(g_fontConfig, pat, &result);

  if (font && (FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch))
  {
    this->font = TTF_OpenFont((char *)file, size);
    if (!this->font)
    {
      DEBUG_ERROR("TTL_OpenFont Failed");
      return false;
    }
  }
  else
  {
    DEBUG_ERROR("Failed to locate the requested font: %s", font_name);
    return false;
  }
  FcPatternDestroy(pat);

  return true;
}

static void lgf_sdl_destroy(LG_FontObj opaque)
{
  struct Inst * this = (struct Inst *)opaque;
  if (this->font)
    TTF_CloseFont(this->font);
  free(this);

  if (--g_initCount == 0)
    TTF_Quit();
}

static LG_FontBitmap * lgf_sdl_render(LG_FontObj opaque, unsigned int fg_color, const char * text)
{
  struct Inst * this = (struct Inst *)opaque;

  SDL_Surface * surface;
  SDL_Color     color;
  color.r = (fg_color & 0xff000000) >> 24;
  color.g = (fg_color & 0x00ff0000) >> 16;
  color.b = (fg_color & 0x0000ff00) >>  8;
  color.a = (fg_color & 0x000000ff) >>  0;

  if (!(surface = TTF_RenderText_Blended(this->font, text, color)))
  {
    DEBUG_ERROR("Failed to render text: %s", TTF_GetError());
    return NULL;
  }

  LG_FontBitmap * out = malloc(sizeof(LG_FontBitmap));
  if (!out)
  {
    SDL_FreeSurface(surface);
    DEBUG_ERROR("Failed to allocate memory for font bitmap");
    return NULL;
  }

  out->reserved = surface;
  out->width    = surface->w;
  out->height   = surface->h;
  out->bpp      = surface->format->BytesPerPixel;
  out->pixels   = surface->pixels;

  return out;
}

static void lgf_sdl_release(LG_FontObj opaque, LG_FontBitmap * font)
{
  LG_FontBitmap * bitmap = (LG_FontBitmap *)font;
  SDL_FreeSurface(bitmap->reserved);
  free(bitmap);
}

struct LG_Font LGF_SDL =
{
  .name         = "SDL",
  .create       = lgf_sdl_create,
  .destroy      = lgf_sdl_destroy,
  .render       = lgf_sdl_render,
  .release      = lgf_sdl_release
};