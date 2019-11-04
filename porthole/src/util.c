/*
Looking Glass - KVM FrameRelay (KVMFR) Client
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

#include "porthole/util.h"
#include "common/debug.h"

#include <string.h>

void porthole_copy_mem_to_map(void * src, PortholeMap * dst, size_t len, off_t off)
{
  if (off + len > dst->size)
    DEBUG_FATAL("Attempt to write beyond the length of destination mapping");

  /* find the start segment */
  PortholeSegment * seg = &dst->segments[0];
  while(off)
  {
    if (seg->size > off)
      break;

    off -= seg->size;
    ++seg;
  }

  /* copy into each segment until the length has been satisfied */
  while(len)
  {
    char * buf   = (char *)seg->data + off;
    size_t avail = seg->size - off;
    off = 0;

    if (avail > len)
      avail = len;

    memcpy(buf, src, avail);

    src = (char *)src + avail;
    len -= avail;
    ++seg;
  }
}

void porthole_copy_map_to_mem(PortholeMap * src, void * dst, size_t len, off_t off)
{
  if (off + len > src->size)
    DEBUG_FATAL("Attempt to read beyond the length of the source mapping");

  /* find the start segment */
  PortholeSegment * seg = &src->segments[0];
  while(off)
  {
    if (seg->size > off)
      break;

    off -= seg->size;
    ++seg;
  }

  /* copy from each segment until the length has been satisfied */
  while(len)
  {
    char * buf = (char *)seg->data + off;
    size_t avail = seg->size - off;
    off = 0;

    if (avail > len)
      avail = len;

    memcpy(dst, buf, avail);

    dst  = (char *)dst + avail;
    len -= avail;
    ++seg;
  }
}

void porthole_copy_map_to_map(PortholeMap * src, PortholeMap * dst, size_t len, off_t src_off, off_t dst_off)
{
  if (src_off + len > src->size)
    DEBUG_FATAL("Attempt to read beyond th elength of the source mapping");

  if (dst_off + len > dst->size)
    DEBUG_FATAL("Attempt to write beyond the length of the destination mapping");

  /* find the start segments */
  PortholeSegment * src_seg = &src->segments[0];
  while(src_off)
  {
    if (src_seg->size > src_off)
      break;

    src_off -= src_seg->size;
    ++src_seg;
  }

  PortholeSegment * dst_seg = &dst->segments[0];
  while(dst_off)
  {
    if (dst_seg->size > dst_off)
      break;

    dst_off -= dst_seg->size;
    ++dst_seg;
  }

  while(len)
  {
    char * src_buf   = (char *)src_seg->data + src_off;
    char * dst_buf   = (char *)dst_seg->data + dst_off;
    size_t src_avail = src_seg->size - src_off;
    size_t dst_avail = dst_seg->size - dst_off;
    src_off = 0;
    dst_off = 0;

    size_t avail = src_avail > dst_avail ? dst_avail : src_avail;
    if (avail > len)
      avail = len;

    memcpy(dst_buf, src_buf, avail);

    src_avail -= avail;
    dst_avail -= avail;

    if (src_avail == 0)
      ++src_seg;
    else
      src_off = src_avail;

    if (dst_avail == 0)
      ++dst_seg;
    else
      dst_off = dst_avail;

    len -= avail;
  }
}