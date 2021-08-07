/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

#include "common/rects.h"

#include <stdlib.h>

struct Corner
{
  int x;
  int y;
  int delta;
};

struct Edge
{
  int x;
  int delta;
};

static int cornerCompare(const void * a_, const void * b_)
{
  const struct Corner * a = a_;
  const struct Corner * b = b_;

  if (a->y < b->y) return -1;
  if (a->y > b->y) return +1;
  if (a->x < b->x) return -1;
  if (a->x > b->x) return +1;
  return 0;
}

inline static void rectsBufferCopy(FrameDamageRect * rects, int count,
  uint8_t * dst, int dstStride, int height,
  const uint8_t * src, int srcStride, void * opaque,
  void (*rowCopyStart)(int y, void * opaque),
  void (*rowCopyFinish)(int y, void * opaque))
{
  const int cornerCount = 4 * count;
  struct Corner corners[cornerCount];

  for (int i = 0; i < count; ++i)
  {
    FrameDamageRect * rect = rects + i;
    corners[4 * i + 0] = (struct Corner) {
      .x = rect->x, .y = rect->y, .delta = 1
    };
    corners[4 * i + 1] = (struct Corner) {
      .x = rect->x + rect->width, .y = rect->y, .delta = -1
    };
    corners[4 * i + 2] = (struct Corner) {
      .x = rect->x, .y = rect->y + rect->height, .delta = -1
    };
    corners[4 * i + 3] = (struct Corner) {
      .x = rect->x + rect->width, .y = rect->y + rect->height, .delta = 1
    };
  }
  qsort(corners, cornerCount, sizeof(struct Corner), cornerCompare);

  struct Edge active_[2][cornerCount];
  struct Edge change[cornerCount];
  int prev_y = 0;
  int activeRow = 0;
  int actives = 0;

  for (int rs = 0;;)
  {
    int y = corners[rs].y;
    int re = rs;
    while (re < cornerCount && corners[re].y == y)
      ++re;

    if (y > height)
      y = height;

    int changes = 0;
    for (int i = rs; i < re; )
    {
      int x = corners[i].x;
      int delta = 0;
      while (i < re && corners[i].x == x)
        delta += corners[i++].delta;
      change[changes++] = (struct Edge) { .x = x, .delta = delta };
    }

    if (rowCopyStart)
      rowCopyStart(y, opaque);

    struct Edge * active = active_[activeRow];
    int x1 = 0;
    int in_rect = 0;
    for (int i = 0; i < actives; ++i)
    {
      if (!in_rect)
        x1 = active[i].x;
      in_rect += active[i].delta;
      if (!in_rect)
        rectCopyUnaligned(dst, src, prev_y, y, x1 * 4, dstStride, srcStride,
            (active[i].x - x1) * 4);
    }

    if (re >= cornerCount || y == height)
      break;

    if (rowCopyFinish)
      rowCopyFinish(y, opaque);

    struct Edge * new = active_[activeRow ^ 1];
    int ai = 0;
    int ci = 0;
    int ni = 0;

    while (ai < actives && ci < changes)
    {
      if (active[ai].x < change[ci].x)
        new[ni++] = active[ai++];
      else if (active[ai].x > change[ci].x)
        new[ni++] = change[ci++];
      else
      {
        active[ai].delta += change[ci++].delta;
        if (active[ai].delta != 0)
          new[ni++] = active[ai];
        ++ai;
      }
    }

    // only one of (actives - ai) and (changes - ci) will be non-zero.
    memcpy(new + ni, active + ai, (actives - ai) * sizeof(struct Edge));
    memcpy(new + ni, change + ci, (changes - ci) * sizeof(struct Edge));
    ni += actives - ai;
    ni += changes - ci;

    actives = ni;
    prev_y = y;
    rs = re;
    activeRow ^= 1;
  }
}

struct ToFramebufferData
{
  FrameBuffer * frame;
  int stride;
};

static void fbRowFinish(int y, void * opaque)
{
  struct ToFramebufferData * data = opaque;
  framebuffer_set_write_ptr(data->frame, y * data->stride);
}

void rectsBufferToFramebuffer(FrameDamageRect * rects, int count,
  FrameBuffer * frame, int dstStride, int height,
  const uint8_t * src, int srcStride)
{
  struct ToFramebufferData data = { .frame = frame, .stride = dstStride };
  rectsBufferCopy(rects, count, framebuffer_get_data(frame), dstStride, height,
    src, srcStride, &data, NULL, fbRowFinish);
  framebuffer_set_write_ptr(frame, height * dstStride);
}

struct FromFramebufferData
{
  const FrameBuffer * frame;
  int stride;
};

static void fbRowStart(int y, void * opaque)
{
  struct FromFramebufferData * data = opaque;
  framebuffer_wait(data->frame, y * data->stride);
}

void rectsFramebufferToBuffer(FrameDamageRect * rects, int count,
  uint8_t * dst, int dstStride, int height,
  const FrameBuffer * frame, int srcStride)
{
  struct FromFramebufferData data = { .frame = frame, .stride = srcStride };
  rectsBufferCopy(rects, count, dst, dstStride, height,
    framebuffer_get_buffer(frame), srcStride, &data, fbRowStart, NULL);
}
