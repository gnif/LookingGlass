/*
KVMGFX Client - A KVM Client for VGA Passthrough
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
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <tmmintrin.h>
#include <immintrin.h>

#include "debug.h"

static inline void memcpySSE(void * dst, const void * src, size_t length)
{
  // check if we can't perform an aligned copy
  if (((uintptr_t)src & 0xF) != ((uintptr_t)dst & 0xF))
  {

    static bool unalignedDstWarn = false;
    if (!unalignedDstWarn)
    {
      DEBUG_WARN("Memcpy64 unable to perform aligned copy, performance will suffer");
      unalignedDstWarn = true;
    }

    // fallback to system memcpy
    memcpy(dst, src, length);
    return;
  }

  // check if the source needs alignment
  {
    uint8_t * _src = (uint8_t *)src;
    unsigned int count = (16 - ((uintptr_t)src & 0xF)) & 0xF;

    static bool unalignedSrcWarn = false;
    if (count > 0)
    {
      if (!unalignedSrcWarn)
      {
        DEBUG_WARN("Memcpy64 unaligned source, performance will suffer");
        unalignedSrcWarn = true;
      }

      uint8_t * _dst = (uint8_t *)dst;
      for (unsigned int i = count; i > 0; --i)
        *_dst++ = *_src++;
      src = _src;
      dst = _dst;
      length -= count;
    }
  }

  __m128i * _src = (__m128i *)src;
  __m128i * _dst = (__m128i *)dst;
  __m128i * _end = (__m128i *)src + (length / 16);
  for (; _src != _end; _src += 8, _dst += 8)
  {
    _mm_prefetch(((char *)_src + 8 ), _MM_HINT_NTA);
    _mm_prefetch(((char *)_src + 9 ), _MM_HINT_NTA);
    _mm_prefetch(((char *)_src + 10), _MM_HINT_NTA);
    _mm_prefetch(((char *)_src + 11), _MM_HINT_NTA);

    __m128i v0 = _mm_load_si128(_src + 0);
    __m128i v1 = _mm_load_si128(_src + 1);
    __m128i v2 = _mm_load_si128(_src + 2);
    __m128i v3 = _mm_load_si128(_src + 3);
    __m128i v4 = _mm_load_si128(_src + 4);
    __m128i v5 = _mm_load_si128(_src + 5);
    __m128i v6 = _mm_load_si128(_src + 6);
    __m128i v7 = _mm_load_si128(_src + 7);

    _mm_stream_si128(_dst + 0, v0);
    _mm_stream_si128(_dst + 1, v1);
    _mm_stream_si128(_dst + 2, v2);
    _mm_stream_si128(_dst + 3, v3);
    _mm_stream_si128(_dst + 4, v4);
    _mm_stream_si128(_dst + 5, v5);
    _mm_stream_si128(_dst + 6, v6);
    _mm_stream_si128(_dst + 7, v7);
  }
}