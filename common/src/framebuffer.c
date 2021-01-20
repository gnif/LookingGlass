/*
KVMGFX Client - A KVM Client for VGA Passthrough
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
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

#include "common/framebuffer.h"
#include "common/debug.h"

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

void framebuffer_wait(const FrameBuffer * frame, size_t size)
{
  while(atomic_load_explicit(&frame->wp, memory_order_acquire) < size)
  {
    int spinCount = 0;
    while(frame->wp < size)
    {
      if (++spinCount == FB_SPIN_LIMIT)
        return;
      usleep(1);
    }
  }
}

bool framebuffer_read(const FrameBuffer * frame, void * restrict dst,
    size_t dstpitch, size_t height, size_t width, size_t bpp, size_t pitch)
{
  uint8_t * restrict d     = (uint8_t*)dst;
  uint_least32_t rp        = 0;
  size_t         y         = 0;
  const size_t   linewidth = width * bpp;

  while(y < height)
  {
    uint_least32_t wp;
    int spinCount = 0;

    /* spinlock */
    wp = atomic_load_explicit(&frame->wp, memory_order_acquire);
    while(wp - rp < linewidth)
    {
      if (++spinCount == FB_SPIN_LIMIT)
        return false;

      usleep(1);
      wp = atomic_load_explicit(&frame->wp, memory_order_acquire);
    }

    /* copy any unaligned bytes */
    const uint8_t * src = frame->data + rp;
    const size_t unaligned = (uintptr_t)src & 0xF;
    if (unaligned)
    {
      memcpy(d, src, unaligned);
      src += unaligned;
      d   += unaligned;
    }

    const size_t blocks = (linewidth - unaligned) / 64;
    const size_t left   = (linewidth - unaligned) % 64;

    _mm_mfence();
    __m128i * restrict s = (__m128i *)src;
    for(int i = 0; i < blocks; ++i)
    {
      __m128i *_d = (__m128i *)d;
      __m128i *_s = (__m128i *)s;
      __m128i v1 = _mm_stream_load_si128(_s + 0);
      __m128i v2 = _mm_stream_load_si128(_s + 1);
      __m128i v3 = _mm_stream_load_si128(_s + 2);
      __m128i v4 = _mm_stream_load_si128(_s + 3);

      _mm_storeu_si128(_d + 0, v1);
      _mm_storeu_si128(_d + 1, v2);
      _mm_storeu_si128(_d + 2, v3);
      _mm_storeu_si128(_d + 3, v4);

      d += 64;
      s += 4;
    }

    if (left)
    {
      memcpy(d, s, left);
      d += left;
    }

    rp += pitch;
    d  += dstpitch - linewidth;
    ++y;
  }

  return true;
}

bool framebuffer_read_fn(const FrameBuffer * frame, size_t height, size_t width,
    size_t bpp, size_t pitch, FrameBufferReadFn fn, void * opaque)
{
  uint_least32_t rp        = 0;
  size_t         y         = 0;
  const size_t   linewidth = width * bpp;

  while(y < height)
  {
    uint_least32_t wp;
    int spinCount = 0;

    /* spinlock */
    wp = atomic_load_explicit(&frame->wp, memory_order_acquire);
    while(wp - rp < linewidth)
    {
      if (++spinCount == FB_SPIN_LIMIT)
        return false;

      usleep(1);
      wp = atomic_load_explicit(&frame->wp, memory_order_acquire);
    }

    if (!fn(opaque, frame->data + rp, linewidth))
      return false;

    rp += pitch;
    ++y;
  }

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
  return true;
}
