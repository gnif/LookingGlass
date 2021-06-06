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

#include <stdlib.h>
#include <stdbool.h>

#include "interface/font.h"
#include "common/debug.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

static int        g_initCount  = 0;
static FcConfig * g_fontConfig = NULL;
static FT_Library g_ft;

struct Inst
{
  FT_Face face;
  unsigned int height;
};

static bool lgf_freetype_create(LG_FontObj * opaque, const char * font_name, unsigned int size)
{
  bool ret = false;

  if (g_initCount == 0)
  {
    if (FT_Init_FreeType(&g_ft))
    {
      DEBUG_ERROR("FT_Init_FreeType Failed");
      goto fail;
    }

    g_fontConfig = FcInitLoadConfigAndFonts();
    if (!g_fontConfig)
    {
      DEBUG_ERROR("FcInitLoadConfigAndFonts Failed");
      goto fail_init;
    }
  }

  *opaque = malloc(sizeof(struct Inst));
  if (!*opaque)
  {
    DEBUG_INFO("Failed to allocate %lu bytes", sizeof(struct Inst));
    goto fail_config;
  }

  memset(*opaque, 0, sizeof(struct Inst));
  struct Inst * this = (struct Inst *)*opaque;

  if (!font_name)
    font_name = "FreeMono";

  FcPattern * pat = FcNameParse((const FcChar8*)font_name);
  if (!pat)
  {
    DEBUG_ERROR("FCNameParse failed");
    goto fail_opaque;
  }

  FcConfigSubstitute(g_fontConfig, pat, FcMatchPattern);
  FcDefaultSubstitute(pat);
  FcResult result;
  FcChar8 * file = NULL;
  FcPattern * match = FcFontMatch(g_fontConfig, pat, &result);

  if (!match)
  {
    DEBUG_ERROR("FcFontMatch Failed");
    goto fail_parse;
  }

  if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
  {
    if (FT_New_Face(g_ft, (char *) file, 0, &this->face))
    {
      DEBUG_ERROR("FT_New_Face Failed");
      goto fail_match;
    }

    if (FT_Select_Charmap(this->face, ft_encoding_unicode))
    {
      DEBUG_ERROR("FT_Select_Charmap failed");
      FT_Done_Face(this->face);
      goto fail_match;
    }

    FT_Set_Pixel_Sizes(this->face, 0, size);
    this->height = size;
  }
  else
  {
    DEBUG_ERROR("Failed to locate the requested font: %s", font_name);
    goto fail_match;
  }

  ++g_initCount;
  ret = true;

fail_match:
  FcPatternDestroy(match);

fail_parse:
  FcPatternDestroy(pat);

  if (ret)
    return true;

fail_opaque:
  free(this);
  *opaque = NULL;

fail_config:
  if (g_initCount == 0)
  {
    FcConfigDestroy(g_fontConfig);
    g_fontConfig = NULL;
  }

fail_init:
  if (g_initCount == 0)
    FT_Done_FreeType(g_ft);

fail:
  return false;
}

static void lgf_freetype_destroy(LG_FontObj opaque)
{
  struct Inst * this = (struct Inst *)opaque;

  if (this->face)
    FT_Done_Face(this->face);
  free(this);

  if (--g_initCount == 0)
  {
    FcConfigDestroy(g_fontConfig);
    g_fontConfig = NULL;

    FT_Done_FreeType(g_ft);
  }
}

// A very simple UTF-8 decoder that assumes the input is valid.
static unsigned int utf8_decode(const char * str)
{
  const unsigned char * ptr = (const unsigned char *) str;
  // Handle the 4 byte case: 1111 0xxx 10xx xxxx 10xx xxxx 10xx xxxx.
  if ((*ptr & 0xf8) == 0xf0)
    return (ptr[0] & 0x07) << 18 | (ptr[1] & 0x3f) << 12 | (ptr[2] & 0x3f) << 6 | (ptr[3] & 0x3f);
  // Handle the 3 byte case: 1110 xxxx 10xx xxxx 10xx xxxx.
  else if ((*ptr & 0xf0) == 0xe0)
    return (ptr[0] & 0x0f) << 12 | (ptr[1] & 0x3f) << 6 | (ptr[2] & 0x3f);
  // Handle the 2 byte case: 110x xxxx 10xx xxxx.
  else if ((*ptr & 0xe0) == 0xc0)
    return (ptr[0] & 0x1f) << 6 | (ptr[1] & 0x3f);
  // Everything else is the 1 byte case.
  else
    return *ptr;
}

// Return the length of the current UTF-8 character. Assumes the input is valid.
static unsigned int utf8_advance(const char * str)
{
  const unsigned char * ptr = (const unsigned char *) str;
  // 4 byte case starts with 1111 0xxx.
  if ((*ptr & 0xf8) == 0xf0)
    return 4;
  // 3 byte case starts with 1110 xxxx.
  else if ((*ptr & 0xf0) == 0xe0)
    return 3;
  // 2 byte case starts with 110x xxxx.
  else if ((*ptr & 0xe0) == 0xc0)
    return 2;
  // Everything else is the 1 byte case.
  else
    return 1;
}

static LG_FontBitmap * lgf_freetype_render(LG_FontObj opaque, unsigned int fg_color, const char * text)
{
  struct Inst * this = (struct Inst *)opaque;

  int width = 0;
  int row = 0;
  int rowWidth = 0;
  int topAscend = 0;
  int bottomDescend = 0;

  for (const char * ptr = text; *ptr; ptr += utf8_advance(ptr))
  {
    unsigned int ch = utf8_decode(ptr);
    if (ch == '\n')
    {
      if (!ptr[1])
        break;
      if (rowWidth > width)
        width = rowWidth;
      rowWidth = bottomDescend = 0;
      ++row;
    }
    else if (FT_Load_Char(this->face, ch, FT_LOAD_RENDER))
    {
      DEBUG_ERROR("Failed to load character: %c", *ptr);
      return NULL;
    }
    else
    {
      FT_GlyphSlot glyph = this->face->glyph;
      rowWidth += glyph->advance.x / 64;

      int descend = glyph->bitmap.rows - glyph->bitmap_top;
      if (descend > bottomDescend)
        bottomDescend = descend;
      if (row == 0 && glyph->bitmap_top > topAscend)
        topAscend = glyph->bitmap_top;
    }
  }

  if (rowWidth > width)
    width = rowWidth;

  int height = topAscend + this->height * row + bottomDescend;
  uint32_t * pixels = calloc(width * height, sizeof(uint32_t));

  if (!pixels)
  {
    DEBUG_ERROR("Failed to allocate memory for font pixels");
    return NULL;
  }

  int baseline = topAscend;
  int x = 0;

  unsigned int r = (fg_color & 0xff000000) >> 24;
  unsigned int g = (fg_color & 0x00ff0000) >> 16;
  unsigned int b = (fg_color & 0x0000ff00) >>  8;
  uint32_t color = (r << 0) | (g << 8) | (b << 16);

  for (const char * ptr = text; *ptr; ptr += utf8_advance(ptr))
  {
    unsigned int ch = utf8_decode(ptr);
    if (ch == '\n')
    {
      baseline += this->height;
      x = 0;
    }
    else if (FT_Load_Char(this->face, ch, FT_LOAD_RENDER))
    {
      DEBUG_ERROR("Failed to load character: U+%x", ch);
      return NULL;
    }
    else
    {
      FT_GlyphSlot glyph = this->face->glyph;
      int start = baseline - glyph->bitmap_top;
      int pitch = width;

      if (glyph->bitmap.pitch < 0)
      {
        start += glyph->bitmap.rows - 1;
        pitch = -pitch;
      }

      for (int i = 0; i < glyph->bitmap.rows; ++i)
        for (int j = 0; j < glyph->bitmap.width; ++j)
          pixels[(start + i) * pitch + x + j + glyph->bitmap_left] = color |
              (uint32_t)glyph->bitmap.buffer[i * glyph->bitmap.pitch + j] << 24;

      x += glyph->advance.x / 64;
    }
  }

  LG_FontBitmap * out = malloc(sizeof(LG_FontBitmap));
  if (!out)
  {
    free(pixels);
    DEBUG_ERROR("Failed to allocate memory for font bitmap");
    return NULL;
  }

  out->width    = width;
  out->height   = height;
  out->bpp      = 4;
  out->pixels   = (uint8_t *) pixels;

  return out;
}

static void lgf_freetype_release(LG_FontObj opaque, LG_FontBitmap * font)
{
  LG_FontBitmap * bitmap = (LG_FontBitmap *)font;
  free(bitmap->pixels);
  free(bitmap);
}

struct LG_Font LGF_freetype =
{
  .name         = "freetype",
  .create       = lgf_freetype_create,
  .destroy      = lgf_freetype_destroy,
  .render       = lgf_freetype_render,
  .release      = lgf_freetype_release
};
