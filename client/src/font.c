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

#include "font.h"
#include "font_platform.h"

#include "common/debug.h"
#include "common/filesystem.h"
#include "common/locking.h"
#include "common/stringutils.h"

#include "cimgui.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STBRP_STATIC
#define STB_RECT_PACK_IMPLEMENTATION
#include "repos/gui/cimgui/imgui/imstb_rectpack.h"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "repos/gui/cimgui/imgui/imstb_truetype.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

typedef struct UIFontGlyphSet
{
  uint32_t * data;
  size_t     count;
  size_t     capacity;
}
UIFontGlyphSet;

typedef struct UIFontFace
{
  char             * file;
  void             * data;
  size_t             dataSize;
  int                index;
  ImVector_ImWchar   ranges;
}
UIFontFace;

static FontPlatform  * UIFontPlatform         = NULL;
static UIFontGlyphSet  UIFontChars            = { 0 };
static UIFontGlyphSet  UIFontRequested        = { 0 };
static UIFontGlyphSet  UIFontResolved         = { 0 };
static UIFontFace     * UIFontFaces           = NULL;
static size_t           UIFontFaceCount       = 0;
static LG_Lock          UIFontLock;
static bool             UIFontLockInitialized = false;
static bool             UIFontSelected        = false;

static uint16_t font_readU16(const unsigned char * data)
{
  return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t font_readU32(const unsigned char * data)
{
  return
    ((uint32_t)data[0] << 24) |
    ((uint32_t)data[1] << 16) |
    ((uint32_t)data[2] <<  8) |
     (uint32_t)data[3];
}

static bool font_getSfntOffset(const unsigned char * data, size_t size,
    int fontIndex, size_t * offset)
{
  if (!data || size < 12 || size > INT_MAX || fontIndex < 0)
    return false;

  size_t         fontOffset = 0;
  const uint32_t signature  = font_readU32(data);
  switch (signature)
  {
    case 0x00010000: // TrueType 1.0
    case 0x74727565: // true
    case 0x74797031: // typ1
    case 0x4F54544F: // OTTO
      if (fontIndex != 0)
        return false;
      break;

    case 0x74746366: // ttcf
    {
      const uint32_t version   = font_readU32(data + 4);
      const uint32_t fontCount = font_readU32(data + 8);
      if ((version != 0x00010000 && version != 0x00020000) ||
          (uint32_t)fontIndex >= fontCount ||
          fontCount > (size - 12) / 4)
        return false;

      fontOffset = font_readU32(data + 12 + (size_t)fontIndex * 4);
      if (fontOffset > size - 12 || fontOffset > INT_MAX)
        return false;
      break;
    }

    default:
      return false;
  }

  const uint16_t tableCount = font_readU16(data + fontOffset + 4);
  if (tableCount > (size - fontOffset - 12) / 16)
    return false;

  const unsigned char * table = data + fontOffset + 12;
  for (uint16_t i = 0; i < tableCount; ++i, table += 16)
  {
    const size_t tableOffset = font_readU32(table + 8);
    const size_t tableSize   = font_readU32(table + 12);
    if (tableOffset > size || tableSize > size - tableOffset)
      return false;
  }

  *offset = fontOffset;
  return true;
}

bool font_isStbTruetypeCompatible(
    const void * data, size_t size, int fontIndex)
{
  size_t checkedOffset;
  if (!font_getSfntOffset(data, size, fontIndex, &checkedOffset))
    return false;

  const int stbOffset =
    stbtt_GetFontOffsetForIndex((const unsigned char *)data, fontIndex);
  if (stbOffset < 0 || (size_t)stbOffset != checkedOffset)
    return false;

  stbtt_fontinfo info;
  return stbtt_InitFont(&info, data, stbOffset);
}

static size_t uiFontGlyphLowerBound(
    const UIFontGlyphSet * glyphs, uint32_t glyph)
{
  size_t low  = 0;
  size_t high = glyphs->count;

  while (low < high)
  {
    const size_t middle = low + (high - low) / 2;
    if (glyphs->data[middle] < glyph)
      low = middle + 1;
    else
      high = middle;
  }

  return low;
}

static bool uiFontGlyphReserve(UIFontGlyphSet * glyphs, size_t count)
{
  if (count <= glyphs->capacity)
    return true;

  if (count > SIZE_MAX / sizeof(*glyphs->data))
    return false;

  size_t capacity = glyphs->capacity ? glyphs->capacity : 128;
  while (capacity < count)
  {
    if (capacity > SIZE_MAX / 2)
    {
      capacity = count;
      break;
    }
    capacity *= 2;
  }

  uint32_t * data = realloc(glyphs->data, capacity * sizeof(*data));
  if (!data)
    return false;

  glyphs->data     = data;
  glyphs->capacity = capacity;
  return true;
}

static bool uiFontGlyphAdd(
    UIFontGlyphSet * glyphs, uint32_t glyph, bool * added)
{
  const size_t position = uiFontGlyphLowerBound(glyphs, glyph);
  if (position < glyphs->count && glyphs->data[position] == glyph)
  {
    *added = false;
    return true;
  }

  if (!uiFontGlyphReserve(glyphs, glyphs->count + 1))
    return false;

  memmove(glyphs->data + position + 1, glyphs->data + position,
      (glyphs->count - position) * sizeof(*glyphs->data));
  glyphs->data[position] = glyph;
  ++glyphs->count;
  *added = true;
  return true;
}

static bool uiFontGlyphHas(
    const UIFontGlyphSet * glyphs, uint32_t glyph)
{
  const size_t position = uiFontGlyphLowerBound(glyphs, glyph);
  return position < glyphs->count && glyphs->data[position] == glyph;
}

static void uiFontGlyphFree(UIFontGlyphSet * glyphs)
{
  free(glyphs->data);
  glyphs->data     = NULL;
  glyphs->count    = 0;
  glyphs->capacity = 0;
}

static int uiFontGlyphCompare(const void * lhs, const void * rhs)
{
  const uint32_t left  = *(const uint32_t *)lhs;
  const uint32_t right = *(const uint32_t *)rhs;
  return (left > right) - (left < right);
}

static void uiFontClearFaces(void)
{
  for (size_t i = 0; i < UIFontFaceCount; ++i)
  {
    free(UIFontFaces[i].file);
    free(UIFontFaces[i].data);
    ImVector_ImWchar_UnInit(&UIFontFaces[i].ranges);
  }

  free(UIFontFaces);
  UIFontFaces          = NULL;
  UIFontFaceCount      = 0;
  UIFontResolved.count = 0;
}

static bool uiFontAddRanges(const ImWchar * ranges)
{
  for (; ranges[0]; ranges += 2)
    for (uint32_t glyph = ranges[0]; glyph <= (uint32_t)ranges[1]; ++glyph)
    {
      bool added;
      if (!uiFontGlyphAdd(&UIFontChars, glyph, &added))
        return false;
    }

  return true;
}

static bool uiFontBuildRanges(const uint32_t * glyphs, size_t glyphCount,
    ImVector_ImWchar * ranges)
{
  ImFontGlyphRangesBuilder * builder =
    ImFontGlyphRangesBuilder_ImFontGlyphRangesBuilder();
  if (!builder)
    return false;

  for (size_t i = 0; i < glyphCount; ++i)
    ImFontGlyphRangesBuilder_AddChar(builder, (ImWchar)glyphs[i]);

  ImVector_ImWchar_Init(ranges);
  ImFontGlyphRangesBuilder_BuildRanges(builder, ranges);
  ImFontGlyphRangesBuilder_destroy(builder);
  if (ranges->Size > 1)
    return true;

  ImVector_ImWchar_UnInit(ranges);
  return false;
}

static void * uiFontLoadFile(const char * path, size_t * size)
{
  *size = 0;

  FILE * file = lgOpenFile(path, "rb");
  if (!file)
    return NULL;

  if (fseek(file, 0, SEEK_END) != 0)
    goto fail;

  const long length = ftell(file);
  if (length <= 0 || length > INT_MAX || fseek(file, 0, SEEK_SET) != 0)
    goto fail;

  void * data = malloc((size_t)length);
  if (!data)
    goto fail;

  if (fread(data, 1, (size_t)length, file) != (size_t)length)
  {
    free(data);
    goto fail;
  }

  fclose(file);
  *size = (size_t)length;
  return data;

fail:
  fclose(file);
  return NULL;
}

static FontPlatformFaceResult uiFontAddFace(void * opaque,
    const char * file, int index,
    const uint32_t * glyphs, size_t glyphCount)
{
  (void)opaque;

  size_t   dataSize;
  void   * data = uiFontLoadFile(file, &dataSize);
  if (!data)
  {
    DEBUG_WARN("Failed to read UI font face: %s", file);
    return FONT_PLATFORM_FACE_SKIP;
  }

  if (!font_isStbTruetypeCompatible(data, dataSize, index))
  {
    DEBUG_WARN("Skipping stb-incompatible UI font face: %s (index %d)",
        file, index);
    free(data);
    return FONT_PLATFORM_FACE_SKIP;
  }

  ImVector_ImWchar ranges;
  if (!uiFontBuildRanges(glyphs, glyphCount, &ranges))
  {
    free(data);
    return FONT_PLATFORM_FACE_ERROR;
  }

  char * fileCopy = lg_strdup(file);
  if (!fileCopy ||
      !uiFontGlyphReserve(
        &UIFontResolved, UIFontResolved.count + glyphCount))
  {
    free(fileCopy);
    ImVector_ImWchar_UnInit(&ranges);
    free(data);
    return FONT_PLATFORM_FACE_ERROR;
  }

  UIFontFace * faces = realloc(UIFontFaces,
      (UIFontFaceCount + 1) * sizeof(*faces));
  if (!faces)
  {
    free(fileCopy);
    ImVector_ImWchar_UnInit(&ranges);
    free(data);
    return FONT_PLATFORM_FACE_ERROR;
  }
  UIFontFaces = faces;

  memcpy(UIFontResolved.data + UIFontResolved.count,
      glyphs, glyphCount * sizeof(*glyphs));
  UIFontResolved.count += glyphCount;

  UIFontFace * face = &UIFontFaces[UIFontFaceCount++];
  face->file        = fileCopy;
  face->data        = data;
  face->dataSize    = dataSize;
  face->index       = index;
  face->ranges      = ranges;
  return FONT_PLATFORM_FACE_ACCEPT;
}

static bool uiFontBuildFaces(ImFontAtlas * atlas)
{
  uiFontClearFaces();

  static const ImWchar arrowRanges[] =
  {
    0x2190, 0x2193,
    0,
  };

  if (!uiFontAddRanges(ImFontAtlas_GetGlyphRangesDefault(atlas)) ||
      !uiFontAddRanges(arrowRanges))
    return false;

  size_t unresolved;
  if (!font_platformResolve(UIFontPlatform,
        UIFontChars.data, UIFontChars.count, uiFontAddFace, NULL, &unresolved))
  {
    uiFontClearFaces();
    return false;
  }

  if (UIFontResolved.count)
    qsort(UIFontResolved.data, UIFontResolved.count,
        sizeof(*UIFontResolved.data), uiFontGlyphCompare);

  size_t missingRequested = 0;
  for (size_t i = 0; i < UIFontRequested.count; ++i)
    if (!uiFontGlyphHas(&UIFontResolved, UIFontRequested.data[i]))
      ++missingRequested;

  if (unresolved && missingRequested)
    DEBUG_WARN("%u requested UI glyphs have no usable outline font",
        (unsigned int)missingRequested);

  return UIFontFaceCount > 0;
}

static ImFont * uiFontAddSize(ImFontAtlas * atlas, float size)
{
  ImFont * result = NULL;

  for (size_t i = 0; i < UIFontFaceCount; ++i)
  {
    ImFontConfig * config = ImFontConfig_ImFontConfig();
    if (!config)
      return NULL;

    config->MergeMode            = result != NULL;
    config->FontDataOwnedByAtlas = false;
    config->FontNo               = UIFontFaces[i].index;

    ImFont * font = ImFontAtlas_AddFontFromMemoryTTF(atlas,
        UIFontFaces[i].data, (int)UIFontFaces[i].dataSize,
        size, config, UIFontFaces[i].ranges.Data);
    ImFontConfig_destroy(config);

    if (!font)
    {
      DEBUG_WARN("Failed to add UI font face: %s", UIFontFaces[i].file);
      continue;
    }

    if (!result)
      result = font;
  }

  return result;
}

bool font_init(void)
{
  if (UIFontPlatform)
    return true;

  FontPlatform * platform;
  if (!font_platformInit(&platform))
    return false;

  UIFontPlatform = platform;
  LG_LOCK_INIT(UIFontLock);
  UIFontLockInitialized = true;
  return true;
}

char * font_get(const char * fontName)
{
  if (!UIFontPlatform || !fontName)
    return NULL;

  LG_LOCK(UIFontLock);
  char * path = font_platformSelect(UIFontPlatform, fontName);
  if (path)
    UIFontSelected = true;
  LG_UNLOCK(UIFontLock);
  return path;
}

bool font_addText(const char * text)
{
  if (!UIFontPlatform || !text)
    return false;

  bool changed = false;
  LG_LOCK(UIFontLock);

  const char * pos = text;
  while (*pos)
  {
    unsigned int glyph;
    const int    length = igImTextCharFromUtf8(&glyph, pos, NULL);
    if (length <= 0)
      break;
    pos += length;

    if (glyph < 0x20 || glyph > IM_UNICODE_CODEPOINT_MAX)
      continue;

    bool added;
    if (!uiFontGlyphAdd(&UIFontChars, glyph, &added))
    {
      DEBUG_ERROR("Failed to record a UI glyph");
      break;
    }
    changed |= added;

    if (!uiFontGlyphAdd(&UIFontRequested, glyph, &added))
    {
      DEBUG_ERROR("Failed to record a requested UI glyph");
      break;
    }
  }

  LG_UNLOCK(UIFontLock);
  return changed;
}

bool font_buildAtlas(ImFontAtlas * atlas, float size, ImFont ** large)
{
  if (!UIFontPlatform || !UIFontSelected || !atlas || !large || size <= 0.0f)
    return false;

  bool result = false;
  *large      = NULL;
  LG_LOCK(UIFontLock);

  ImFontAtlas_Clear(atlas);
  if (!uiFontBuildFaces(atlas))
  {
    DEBUG_ERROR("Failed to resolve UI fonts");
    goto done;
  }

  if (!uiFontAddSize(atlas, size))
  {
    DEBUG_ERROR("Failed to add the primary UI font");
    goto done;
  }

  *large = uiFontAddSize(atlas, size * 1.3f);
  if (!*large)
  {
    DEBUG_ERROR("Failed to add the large UI font");
    goto done;
  }

  result = true;

done:
  if (!result)
  {
    ImFontAtlas_Clear(atlas);
    uiFontClearFaces();
  }

  LG_UNLOCK(UIFontLock);
  return result;
}

void font_free(void)
{
  if (!UIFontPlatform)
    return;

  if (UIFontLockInitialized)
    LG_LOCK(UIFontLock);

  uiFontClearFaces();
  uiFontGlyphFree(&UIFontChars);
  uiFontGlyphFree(&UIFontRequested);
  uiFontGlyphFree(&UIFontResolved);

  FontPlatform * platform = UIFontPlatform;
  UIFontPlatform          = NULL;
  UIFontSelected          = false;

  if (UIFontLockInitialized)
  {
    LG_UNLOCK(UIFontLock);
    LG_LOCK_FREE(UIFontLock);
    UIFontLockInitialized = false;
  }

  font_platformFree(platform);
}
