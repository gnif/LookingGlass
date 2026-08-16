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

#include "WCWrite.h"

#include <atomic>
#include <immintrin.h>
#include <intrin.h>
#include <stdint.h>
#include <string.h>

namespace WCWrite
{
  namespace Detail
  {
    void CopyAVX2(void * destination, const void * source, size_t size);
  }

  namespace
  {
    using CopyFn = void (*)(void *, const void *, size_t);
    static constexpr unsigned __int64 AVX_XSTATE_MASK = 0x6U;

    void CopyFallback(void * destination, const void * source, size_t size)
    {
      memcpy(destination, source, size);
    }

#if defined(_M_IX86) || defined(_M_X64)
    __declspec(noinline) void CopySSE2(
      void * destination, const void * source, size_t size)
    {
      uint8_t * dst       = static_cast<uint8_t *>(destination);
      const uint8_t * src = static_cast<const uint8_t *>(source);

      const size_t prefix =
        (16U - (reinterpret_cast<uintptr_t>(dst) & 15U)) & 15U;
      if (prefix)
      {
        const size_t copy = prefix < size ? prefix : size;
        memcpy(dst, src, copy);
        dst  += copy;
        src  += copy;
        size -= copy;
      }

      while (size >= 64U)
      {
        const __m128i v0 = _mm_loadu_si128(
          reinterpret_cast<const __m128i *>(src +  0));
        const __m128i v1 = _mm_loadu_si128(
          reinterpret_cast<const __m128i *>(src + 16));
        const __m128i v2 = _mm_loadu_si128(
          reinterpret_cast<const __m128i *>(src + 32));
        const __m128i v3 = _mm_loadu_si128(
          reinterpret_cast<const __m128i *>(src + 48));
        __m128i * output = reinterpret_cast<__m128i *>(dst);
        _mm_stream_si128(output + 0, v0);
        _mm_stream_si128(output + 1, v1);
        _mm_stream_si128(output + 2, v2);
        _mm_stream_si128(output + 3, v3);
        dst  += 64U;
        src  += 64U;
        size -= 64U;
      }

      while (size >= 16U)
      {
        const __m128i value = _mm_loadu_si128(
          reinterpret_cast<const __m128i *>(src));
        _mm_stream_si128(reinterpret_cast<__m128i *>(dst), value);
        dst  += 16U;
        src  += 16U;
        size -= 16U;
      }

      if (size)
        memcpy(dst, src, size);
    }

    CopyFn SelectCopy()
    {
      int registers[4] = {};
      __cpuid(registers, 0);
      const int maximumLeaf = registers[0];
      if (maximumLeaf < 1)
        return &CopyFallback;

      __cpuidex(registers, 1, 0);
      const bool sse2    = (registers[3] & (1 << 26)) != 0;
      const bool avx     = (registers[2] & (1 << 28)) != 0;
      const bool osxsave = (registers[2] & (1 << 27)) != 0;

      if (maximumLeaf >= 7 && avx && osxsave &&
        (_xgetbv(0) & AVX_XSTATE_MASK) == AVX_XSTATE_MASK)
      {
        __cpuidex(registers, 7, 0);
        if ((registers[1] & (1 << 5)) != 0)
          return &Detail::CopyAVX2;
      }

      return sse2 ? &CopySSE2 : &CopyFallback;
    }
#else
    CopyFn SelectCopy()
    {
      return &CopyFallback;
    }
#endif
  }

  void Copy(void * destination, const void * source, size_t size)
  {
    if (!size)
      return;

    static const CopyFn copy = SelectCopy();
    copy(destination, source, size);
  }

  void Flush()
  {
#if defined(_M_IX86) || defined(_M_X64)
    _mm_sfence();
#else
    std::atomic_thread_fence(std::memory_order_release);
#endif
  }
}
