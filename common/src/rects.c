/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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
#include "common/util.h"
#include "common/cpuinfo.h"

#include <stdlib.h>
#include <immintrin.h>

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

inline static bool rectIntersects(const FrameDamageRect * r1,
    const FrameDamageRect * r2)
{
  return !(
    r1->x > r2->x + r2->width  ||
    r2->x > r1->x + r1->width  ||
    r1->y > r2->y + r2->height ||
    r2->y > r1->y + r1->height);
}

inline static bool rectContains(const FrameDamageRect * r1,
    const FrameDamageRect * r2)
{
  return !(
    r2->x              < r1->x              ||
    r2->x + r2->width  > r1->x + r1->width  ||
    r2->y              < r1->y              ||
    r2->y + r2->height > r1->y + r1->height);
}

inline static int removeRects(FrameDamageRect * rects, int count,
    bool removed[])
{
  int o = 0;
  for (int i = 0; i < count; ++i)
  {
    if (removed[i] || i == o++)
      continue;

    rects[o-1] = rects[i];
  }

  return o;
}

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

inline static void rectsBufferCopy(FrameDamageRect * rects, int count, int bpp,
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
        rectCopyUnaligned(dst, src, prev_y, y, x1 * bpp, dstStride, srcStride,
            (active[i].x - x1) * bpp);
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
  int pitch;
};

static void fbRowFinish(int y, void * opaque)
{
  struct ToFramebufferData * data = opaque;
  framebuffer_set_write_ptr(data->frame, y * data->pitch);
}

void rectsBufferToFramebuffer(FrameDamageRect * rects, int count, int bpp,
  FrameBuffer * frame, int dstPitch, int height,
  const uint8_t * src, int srcPitch)
{
  struct ToFramebufferData data = { .frame = frame, .pitch = dstPitch };
  rectsBufferCopy(rects, count, bpp, framebuffer_get_data(frame), dstPitch,
    height, src, srcPitch, &data, NULL, fbRowFinish);
  framebuffer_set_write_ptr(frame, height * dstPitch);
}

struct FromFramebufferData
{
  const FrameBuffer * frame;
  int pitch;
};

static void fbRowStart(int y, void * opaque)
{
  struct FromFramebufferData * data = opaque;
  framebuffer_wait(data->frame, y * data->pitch);
}

void rectsFramebufferToBuffer(FrameDamageRect * rects, int count, int bpp,
  uint8_t * dst, int dstPitch, int height,
  const FrameBuffer * frame, int srcPitch)
{
  struct FromFramebufferData data = { .frame = frame, .pitch = srcPitch };
  rectsBufferCopy(rects, count, bpp, dst, dstPitch, height,
    framebuffer_get_buffer(frame), srcPitch, &data, fbRowStart, NULL);
}

int rectsMergeOverlapping(FrameDamageRect * rects, int count)
{
  if (count == 0)
    return 0;

  bool removed[count];
  bool changed;

  memset(removed, 0, sizeof(removed));

  do
  {
    changed = false;
    for (int i = 0; i < count; ++i)
    {
      if (removed[i])
        continue;

      for (int j = i + 1; j < count; ++j)
      {
        if (removed[j] || !rectIntersects(rects + i, rects + j))
          continue;

        const uint32_t x2 = max(rects[i].x + rects[i].width,
            rects[j].x + rects[j].width);
        const uint32_t y2 = max(rects[i].y + rects[i].height,
            rects[j].y + rects[j].height);

        rects[i].x      = min(rects[i].x, rects[j].x);
        rects[i].y      = min(rects[i].y, rects[j].y);
        rects[i].width  = x2 - rects[i].x;
        rects[i].height = y2 - rects[i].y;

        removed[j] = true;
        changed    = true;
      }
    }
  }
  while (changed);

  return removeRects(rects, count, removed);
}

int rectsRejectContained(FrameDamageRect * rects, int count)
{
  bool removed[count];
  memset(removed, 0, sizeof(removed));

  for (int i = 0; i < count; ++i)
  {
    if (removed[i])
      continue;

    for (int j = 0; j < count; ++j)
    {
      if (j == i || removed[j])
        continue;

      removed[j] = rectContains(rects + i, rects + j);
    }
  }

  return removeRects(rects, count, removed);
}

static void rectCopyUnaligned_memcpy(
    uint8_t *restrict dst, const uint8_t *restrict src,
    int ystart, int yend, int dx, int dstPitch, int srcPitch, int width)
{
  src += ystart * srcPitch + dx;
  dst += ystart * dstPitch + dx;
  for (int i = ystart; i < yend; ++i)
  {
    memcpy(dst, src, width);
    src += srcPitch;
    dst += dstPitch;
  }
}

#ifdef __clang__
  #pragma clang attribute push (__attribute__((target("avx"))), apply_to=function)
#else
  #pragma GCC push_options
  #pragma GCC target ("avx")
#endif
static void rectCopyUnaligned_avx(
    uint8_t *restrict dst, const uint8_t *restrict src,
    int ystart, int yend, int dx, int dstPitch, int srcPitch, int width)
{
  src += ystart * srcPitch + dx;
  dst += ystart * dstPitch + dx;

  const int align = (32 - ((uintptr_t)dst & 31)) & 31;
  const int nvec  = (width - align) / sizeof(__m256i);
  const int rem   = (width - align) % sizeof(__m256i);

  for (int i = ystart; i < yend; ++i)
  {
    // copy the unaligned bytes
    for(int col = align - 1; col >= 0; --col)
      dst[col] = src[col];

    const __m256i *restrict s = (__m256i*)(src + align);
          __m256i *restrict d = (__m256i*)(dst + align);

    int vec;
    for(vec = nvec; vec > 3; vec -= 4)
    {
      _mm256_stream_si256(d + 0, _mm256_loadu_si256(s + 0));
      _mm256_stream_si256(d + 1, _mm256_loadu_si256(s + 1));
      _mm256_stream_si256(d + 2, _mm256_loadu_si256(s + 2));
      _mm256_stream_si256(d + 3, _mm256_loadu_si256(s + 3));

      s += 4;
      d += 4;
    }

    for(; vec > 0; --vec, ++d, ++s)
      _mm256_stream_si256(d, _mm256_loadu_si256(s));

    // copy any remaining bytes
    for(int col = width - rem; col < width; ++col)
      dst[col] = src[col];

    src += srcPitch;
    dst += dstPitch;
  }
}
#ifdef __clang__
  #pragma clang attribute pop
#else
  #pragma GCC pop_options
#endif

static void _rectCopyUnaligned(
  uint8_t *restrict dst, const uint8_t *restrict src,
    int ystart, int yend, int dx, int dstPitch, int srcPitch, int width)
{
  if (cpuInfo_getFeatures()->avx)
    rectCopyUnaligned = &rectCopyUnaligned_avx;
  else
    rectCopyUnaligned = &rectCopyUnaligned_memcpy;

  return rectCopyUnaligned(
      dst, src, ystart, yend, dx, dstPitch, srcPitch, width);
}

void (*rectCopyUnaligned)(uint8_t * dst, const uint8_t * src,
    int ystart, int yend, int dx, int dstPitch, int srcPitch, int width) =
  &_rectCopyUnaligned;
