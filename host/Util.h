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

#if __MINGW32__
#define INTERLOCKED_AND8 __sync_and_and_fetch
#define INTERLOCKED_OR8 __sync_or_and_fetch
#else
#define INTERLOCKED_OR8 InterlockedOr8
#define INTERLOCKED_AND8 InterlockedAnd8
#endif

class Util
{
public:
  static std::string GetSystemRoot()
  {
    std::string defaultPath;

#if __MINGW32__
    const char  * libPath    = getenv("SystemRoot");
    const size_t  libPathLen = strlen(libPath);
#else
    char  * libPath;
    size_t  libPathLen;
    _dupenv_s(&libPath, &libPathLen, "SystemRoot");
#endif

    if (!libPath)
    {
      DEBUG_ERROR("Unable to get the SystemRoot environment variable");
      return defaultPath;
    }

    if (!libPathLen)
    {
      DEBUG_ERROR("The SystemRoot environment variable is not set");
#ifndef __MINGW32__
      free(libPath);
#endif
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
#ifndef __MINGW32__
    free(libPath);
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
};