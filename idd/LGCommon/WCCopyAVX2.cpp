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

#include "WCCopy.h"

#include <immintrin.h>
#include <stdint.h>
#include <string.h>

namespace WCCopy
{
  namespace Detail
  {
    __declspec(noinline) void CopyAVX2(
      void * destination, const void * source, size_t size)
    {
      uint8_t * dst       = static_cast<uint8_t *>(destination);
      const uint8_t * src = static_cast<const uint8_t *>(source);

      _mm_mfence();

      const size_t prefix =
        (32U - (reinterpret_cast<uintptr_t>(src) & 31U)) & 31U;
      if (prefix)
      {
        const size_t copy = prefix < size ? prefix : size;
        memcpy(dst, src, copy);
        dst  += copy;
        src  += copy;
        size -= copy;
      }

      while (size >= 128U)
      {
        const __m256i * input =
          reinterpret_cast<const __m256i *>(src);
        __m256i * output = reinterpret_cast<__m256i *>(dst);
        const __m256i v0 = _mm256_stream_load_si256(input + 0);
        const __m256i v1 = _mm256_stream_load_si256(input + 1);
        const __m256i v2 = _mm256_stream_load_si256(input + 2);
        const __m256i v3 = _mm256_stream_load_si256(input + 3);
        _mm256_storeu_si256(output + 0, v0);
        _mm256_storeu_si256(output + 1, v1);
        _mm256_storeu_si256(output + 2, v2);
        _mm256_storeu_si256(output + 3, v3);
        dst  += 128U;
        src  += 128U;
        size -= 128U;
      }

      while (size >= 32U)
      {
        const __m256i value = _mm256_stream_load_si256(
          reinterpret_cast<const __m256i *>(src));
        _mm256_storeu_si256(reinterpret_cast<__m256i *>(dst), value);
        dst  += 32U;
        src  += 32U;
        size -= 32U;
      }

      if (size)
        memcpy(dst, src, size);

      _mm_mfence();
      _mm256_zeroupper();
    }
  }
}
