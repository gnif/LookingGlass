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

#define FB_CHUNK_SIZE 1048576

struct stFrameBuffer
{
  atomic_uint_least32_t wp;
  uint8_t               data[0];
};

const size_t FrameBufferStructSize = sizeof(FrameBuffer);

void framebuffer_wait(const FrameBuffer * frame, size_t size)
{
  while(atomic_load_explicit(&frame->wp, memory_order_acquire) != size) {}
}


bool framebuffer_read(const FrameBuffer * frame, void * dst, size_t dstpitch,
    size_t height, size_t width, size_t bpp, size_t pitch)
{
  uint8_t       *d         = (uint8_t*)dst;
  uint_least32_t rp        = 0;
  size_t         y         = 0;
  const size_t   linewidth = width * bpp;
  const size_t   blocks    = linewidth / 16;
  const size_t   left      = linewidth % 16;

  while(y < height)
  {
    uint_least32_t wp;

    /* spinlock */
    do
      wp = atomic_load_explicit(&frame->wp, memory_order_acquire);
    while(wp - rp < pitch);

    __m128i * s = (__m128i *)(frame->data + rp);
    for(int i = 0; i < blocks; ++i, ++s, d += 16)
      _mm_stream_si128((__m128i *)d, _mm_stream_load_si128(s));

    if (left)
      memcpy(d, frame->data + rp + blocks * 16, left);

    rp += pitch;
    d  += dstpitch - blocks * 16;
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

    /* spinlock */
    do
      wp = atomic_load_explicit(&frame->wp, memory_order_acquire);
    while(wp - rp < pitch);

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

bool framebuffer_write(FrameBuffer * frame, const void * src, size_t size)
{
  __m128i * s = (__m128i *)src;
  __m128i * d = (__m128i *)frame->data;
  size_t wp     = 0;

  /* copy in chunks */
  while(size > 63)
  {
    const __m128i v1 = _mm_stream_load_si128(s++);
    const __m128i v2 = _mm_stream_load_si128(s++);
    const __m128i v3 = _mm_stream_load_si128(s++);
    const __m128i v4 = _mm_stream_load_si128(s++);
    _mm_stream_si128(d++, v1);
    _mm_stream_si128(d++, v2);
    _mm_stream_si128(d++, v3);
    _mm_stream_si128(d++, v4);

    size -= 64;
    wp   += 64;

    if (wp % FB_CHUNK_SIZE == 0)
      atomic_store_explicit(&frame->wp, wp, memory_order_release);
  }

  if (size > 47)
  {
    const __m128i v1 = _mm_stream_load_si128(s++);
    const __m128i v2 = _mm_stream_load_si128(s++);
    const __m128i v3 = _mm_stream_load_si128(s++);
    _mm_stream_si128(d++, v1);
    _mm_stream_si128(d++, v2);
    _mm_stream_si128(d++, v3);
    size -= 48;
    wp   += 48;
  }

  if (size > 31)
  {
    const __m128i v1 = _mm_stream_load_si128(s++);
    const __m128i v2 = _mm_stream_load_si128(s++);
    _mm_stream_si128(d++, v1);
    _mm_stream_si128(d++, v2);
    size -= 32;
    wp   += 32;
  }

  if (size > 15)
  {
    const __m128i v1 = _mm_stream_load_si128(s++);
    _mm_stream_si128(d++, v1);
    size -= 16;
    wp   += 16;
  }

  if(size)
  {
    memcpy(frame->data + wp, s, size);
    wp += size;
  }

  atomic_store_explicit(&frame->wp, wp, memory_order_release);
  return true;
}
