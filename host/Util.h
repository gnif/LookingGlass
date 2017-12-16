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
#include <string>
#include <assert.h>
#include <inttypes.h>
#include <tmmintrin.h>

#include "common/debug.h"

#if __MINGW32__
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

class Util
{
public:
  static std::string GetSystemRoot()
  {
    std::string defaultPath;

    const char *libPath = getenv("SystemRoot");

    if (!libPath)
    {
      DEBUG_ERROR("Unable to get the SystemRoot environment variable");
      return defaultPath;
    }

    if (!strlen(libPath))
    {
      DEBUG_ERROR("The SystemRoot environment variable is not set");
      return defaultPath;
    }
#ifdef _WIN64
    defaultPath = std::string(libPath) + "\\System32";
#else
    if (IsWow64())
    {
      defaultPath = std::string(libPath) + "\\Syswow64";
    }
    else
    {
      defaultPath = std::string(libPath) + "\\System32";
    }
#endif
    return defaultPath;
  }

  static inline void BGRAtoRGB(uint8_t * orig, size_t imagesize, uint8_t * dest)
  {
    assert((uintptr_t)orig % 16 == 0);
    assert((uintptr_t)dest % 16 == 0);
    assert(imagesize % 16 == 0);

    __m128i mask_right = _mm_set_epi8
    (
        12,   13,   14,    8,
         9,   10,    4,    5, 
         6,    0,    1,    2,
      -128, -128, -128, -128
    );

    __m128i mask_left = _mm_set_epi8
    (
      -128, -128, -128, -128,
        12,   13,   14,    8,
         9,   10,    4,    5, 
         6,    0,    1,    2
    );


    uint8_t *end = orig + imagesize * 4;
    for (; orig != end; orig += 64, dest += 48)
    {
      _mm_prefetch((char *)(orig + 128), _MM_HINT_NTA);
      _mm_prefetch((char *)(orig + 192), _MM_HINT_NTA);

      __m128i v0 = _mm_shuffle_epi8(_mm_load_si128((__m128i *)&orig[0 ]), mask_right);
      __m128i v1 = _mm_shuffle_epi8(_mm_load_si128((__m128i *)&orig[16]), mask_left );
      __m128i v2 = _mm_shuffle_epi8(_mm_load_si128((__m128i *)&orig[32]), mask_left );
      __m128i v3 = _mm_shuffle_epi8(_mm_load_si128((__m128i *)&orig[48]), mask_left );

      v0 = _mm_alignr_epi8(v1, v0, 4);
      v1 = _mm_alignr_epi8(v2, _mm_slli_si128(v1, 4),  8);
      v2 = _mm_alignr_epi8(v3, _mm_slli_si128(v2, 4), 12);
      
      _mm_stream_si128((__m128i *)&dest[0 ], v0);
      _mm_stream_si128((__m128i *)&dest[16], v1);
      _mm_stream_si128((__m128i *)&dest[32], v2);
    }
  }

  static void DrawCursor(
    const enum CursorType   type,
    const uint8_t         * cursorData,
    const POINT             cursorRect,
    const unsigned int      cursorPitch,
    const POINT             cursorPos,
    FrameInfo             & frame
  )
  {
    const int maxHeight = min(cursorRect.y, (int)frame.height - cursorPos.y);
    const int maxWidth  = min(cursorRect.x, (int)frame.width  - cursorPos.x);

    switch (type)
    {
      case CURSOR_TYPE_COLOR:
      {
        const unsigned int destPitch = frame.stride * 4;
        for (int y = abs(min(0, cursorPos.y)); y < maxHeight; ++y)
          for (int x = abs(min(0, cursorPos.x)); x < maxWidth; ++x)
          {
            uint8_t *src = (uint8_t *)cursorData + (cursorPitch * y) + (x * 4);
            uint8_t *dst = (uint8_t *)frame.buffer + (destPitch * (y + cursorPos.y)) + ((x + cursorPos.x) * 4);

            const unsigned int alpha = src[3] + 1;
            const unsigned int inv = 256 - alpha;
            dst[0] = (uint8_t)((alpha * src[0] + inv * dst[0]) >> 8);
            dst[1] = (uint8_t)((alpha * src[1] + inv * dst[1]) >> 8);
            dst[2] = (uint8_t)((alpha * src[2] + inv * dst[2]) >> 8);
          }
        break;
      }

      case CURSOR_TYPE_MASKED_COLOR:
      {
        for (int y = abs(min(0, cursorPos.y)); y < maxHeight; ++y)
          for (int x = abs(min(0, cursorPos.x)); x < maxWidth; ++x)
          {
            uint32_t *src = (uint32_t *)cursorData + ((cursorPitch / 4) * y) + x;
            uint32_t *dst = (uint32_t *)frame.buffer + (frame.stride * (y + cursorPos.y)) + (x + cursorPos.x);
            if (*src & 0xff000000)
              *dst = 0xff000000 | (*dst ^ *src);
            else *dst = 0xff000000 | *src;
          }
        break;
      }

      case CURSOR_TYPE_MONOCHROME:
      {
        for (int y = abs(min(0, cursorPos.y)); y < maxHeight / 2; ++y)
          for (int x = abs(min(0, cursorPos.x)); x < maxWidth; ++x)
          {
            uint8_t  *srcAnd = (uint8_t  *)cursorData + (cursorPitch * y) + (x / 8);
            uint8_t  *srcXor = srcAnd + cursorPitch * (cursorRect.y / 2);
            uint32_t *dst = (uint32_t *)frame.buffer + (frame.stride * (y + cursorPos.y)) + (x + cursorPos.x);
            const uint8_t mask = 0x80 >> (x % 8);
            const uint32_t andMask = (*srcAnd & mask) ? 0xFFFFFFFF : 0xFF000000;
            const uint32_t xorMask = (*srcXor & mask) ? 0x00FFFFFF : 0x00000000;
            *dst = (*dst & andMask) ^ xorMask;
          }
        break;
      }
    }
  }
};