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
#define FB_CHUNK_SIZE 1024

bool framebuffer_read(const FrameBuffer * frame, void * dst, size_t size)
{
  uint8_t *d  = (uint8_t*)dst;
  uint64_t rp = 0;
  while(rp < size)
  {
    /* spinlock */
    while(rp == frame->wp) { }

    /* copy what we can */
    uint64_t avail = frame->wp - rp;
    avail = avail > size ? size : avail;

    memcpy(d, frame->data + rp, avail);

    rp   += avail;
    d    += avail;
    size -= avail;
  }
  return true;
}

bool framebuffer_read_fn(const FrameBuffer * frame, FrameBufferReadFn fn, size_t size, void * opaque)
{
  uint64_t rp = 0;
  while(rp < size)
  {
    /* spinlock */
    while(rp == frame->wp) { }

    /* copy what we can */
    uint64_t avail = frame->wp - rp;
    avail = avail > size ? size : avail;

    if (!fn(opaque, frame->data + rp, avail))
      return false;

    rp   += avail;
    size -= avail;
  }

  return true;
}

/**
 * Prepare the framebuffer for writing
 */
void framebuffer_prepare(FrameBuffer * frame)
{
  frame->wp = 0;
}

bool framebuffer_write(FrameBuffer * frame, const void * src, size_t size)
{
  /* copy in chunks */
  while(size)
  {
    size_t copy = size < FB_CHUNK_SIZE ? FB_CHUNK_SIZE : size;
    memcpy(frame->data + frame->wp, src, copy);
    __sync_fetch_and_add(&frame->wp, copy);
    size -= copy;
  }
  return true;
}