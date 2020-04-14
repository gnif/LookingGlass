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

#define FB_CHUNK_SIZE 1024

struct stFrameBuffer
{
  atomic_uint_least32_t wp;
  uint8_t               data[0];
};

const size_t FrameBufferStructSize = sizeof(FrameBuffer);

void framebuffer_wait(const FrameBuffer * frame, size_t size)
{
  while(atomic_load_explicit(&frame->wp, memory_order_relaxed) != size) {}
}


bool framebuffer_read(const FrameBuffer * frame, void * dst, size_t dstpitch,
    size_t height, size_t width, size_t bpp, size_t pitch)
{
  uint8_t       *d         = (uint8_t*)dst;
  uint_least32_t rp        = 0;
  size_t         y         = 0;
  const size_t   linewidth = width * bpp;

  while(y < height)
  {
    uint_least32_t wp;

    /* spinlock */
    do
      wp = atomic_load_explicit(&frame->wp, memory_order_relaxed);
    while(wp - rp < pitch);

    memcpy(d, frame->data + rp, linewidth);

    rp += pitch;
    d  += dstpitch;
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
      wp = atomic_load_explicit(&frame->wp, memory_order_relaxed);
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
  atomic_store(&frame->wp, 0);
}

bool framebuffer_write(FrameBuffer * frame, const void * src, size_t size)
{
  /* copy in chunks */
  while(size)
  {
    size_t copy = size < FB_CHUNK_SIZE ? FB_CHUNK_SIZE : size;
    memcpy(frame->data + frame->wp, src, copy);
    atomic_fetch_add(&frame->wp, copy);
    size -= copy;
  }
  return true;
}
