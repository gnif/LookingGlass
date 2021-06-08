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

#include "common/framebuffer.h"
#include "common/debug.h"

//#define FB_PROFILE
#ifdef FB_PROFILE
#include "common/runningavg.h"
#endif

#include <string.h>
#include <stdatomic.h>
#include <emmintrin.h>
#include <smmintrin.h>
#include <unistd.h>

#define FB_CHUNK_SIZE 1048576 // 1MB
#define FB_SPIN_LIMIT 10000   // 10ms

struct stFrameBuffer
{
  atomic_uint_least32_t wp;
  uint8_t               data[0];
};

const size_t FrameBufferStructSize = sizeof(FrameBuffer);

bool framebuffer_wait(const FrameBuffer * frame, size_t size)
{
  while(atomic_load_explicit(&frame->wp, memory_order_acquire) < size)
  {
    int spinCount = 0;
    while(frame->wp < size)
    {
      if (++spinCount == FB_SPIN_LIMIT)
        return false;
      usleep(1);
    }
  }

  return true;
}

bool framebuffer_read(const FrameBuffer * frame, void * restrict dst,
    size_t dstpitch, size_t height, size_t width, size_t bpp, size_t pitch)
{
#ifdef FB_PROFILE
  static RunningAvg ra = NULL;
  static int raCount = 0;
  const uint64_t ts = microtime();
  if (!ra)
    ra = runningavg_new(100);
#endif

  uint8_t * restrict d     = (uint8_t*)dst;
  uint_least32_t rp        = 0;

  // copy in large 1MB chunks if the pitches match
  if (dstpitch == pitch)
  {
    size_t remaining = height * pitch;
    while(remaining)
    {
      const size_t copy = remaining < FB_CHUNK_SIZE ? remaining : FB_CHUNK_SIZE;
      if (!framebuffer_wait(frame, rp + copy))
        return false;

      memcpy(d, frame->data + rp, copy);
      remaining -= copy;
      rp        += copy;
      d         += copy;
    }
  }
  else
  {
    // copy per line to match the pitch of the destination buffer
    const size_t linewidth = width * bpp;
    for(size_t y = 0; y < height; ++y)
    {
      if (!framebuffer_wait(frame, rp + linewidth))
        return false;

      memcpy(d, frame->data + rp, dstpitch);
      rp += linewidth;
      d  += dstpitch;
    }
  }

#ifdef FB_PROFILE
  runningavg_push(ra, microtime() - ts);
  if (++raCount % 100 == 0)
    DEBUG_INFO("Average Copy Time: %.2fμs", runningavg_calc(ra));
#endif

  return true;
}

bool framebuffer_read_fn(const FrameBuffer * frame, size_t height, size_t width,
    size_t bpp, size_t pitch, FrameBufferReadFn fn, void * opaque)
{
#ifdef FB_PROFILE
  static RunningAvg ra = NULL;
  static int raCount = 0;
  const uint64_t ts = microtime();
  if (!ra)
    ra = runningavg_new(100);
#endif

  uint_least32_t rp        = 0;
  size_t         y         = 0;
  const size_t   linewidth = width * bpp;

  while(y < height)
  {
    if (!framebuffer_wait(frame, rp + linewidth))
      return false;

    if (!fn(opaque, frame->data + rp, linewidth))
      return false;

    rp += pitch;
    ++y;
  }

#ifdef FB_PROFILE
  runningavg_push(ra, microtime() - ts);
  if (++raCount % 100 == 0)
    DEBUG_INFO("Average Copy Time: %.2fμs", runningavg_calc(ra));
#endif

  return true;
}

/**
 * Prepare the framebuffer for writing
 */
void framebuffer_prepare(FrameBuffer * frame)
{
  atomic_store_explicit(&frame->wp, 0, memory_order_release);
}

bool framebuffer_write(FrameBuffer * frame, const void * restrict src, size_t size)
{
#ifdef FB_PROFILE
  static RunningAvg ra = NULL;
  static int raCount = 0;
  const uint64_t ts = microtime();
  if (!ra)
    ra = runningavg_new(100);
#endif

  __m128i * restrict s = (__m128i *)src;
  __m128i * restrict d = (__m128i *)frame->data;
  size_t wp = 0;

  _mm_mfence();

  /* copy in chunks */
  while(size > 63)
  {
    __m128i *_d = (__m128i *)d;
    __m128i *_s = (__m128i *)s;
    __m128i v1 = _mm_stream_load_si128(_s + 0);
    __m128i v2 = _mm_stream_load_si128(_s + 1);
    __m128i v3 = _mm_stream_load_si128(_s + 2);
    __m128i v4 = _mm_stream_load_si128(_s + 3);

    _mm_store_si128(_d + 0, v1);
    _mm_store_si128(_d + 1, v2);
    _mm_store_si128(_d + 2, v3);
    _mm_store_si128(_d + 3, v4);

    s    += 4;
    d    += 4;
    size -= 64;
    wp   += 64;

    if (wp % FB_CHUNK_SIZE == 0)
      atomic_store_explicit(&frame->wp, wp, memory_order_release);
  }

  if(size)
  {
    memcpy(frame->data + wp, s, size);
    wp += size;
  }

  atomic_store_explicit(&frame->wp, wp, memory_order_release);

#ifdef FB_PROFILE
  runningavg_push(ra, microtime() - ts);
  if (++raCount % 100 == 0)
    DEBUG_INFO("Average Copy Time: %.2fμs", runningavg_calc(ra));
#endif

  return true;
}
