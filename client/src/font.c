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

#include <limits.h>
#include <stdint.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "repos/cimgui/imgui/imstb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

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

  size_t fontOffset = 0;
  const uint32_t signature = font_readU32(data);
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
      const uint32_t version = font_readU32(data + 4);
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
