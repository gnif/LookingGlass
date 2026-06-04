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

#include <stdint.h>

struct WaylandScale
{
  int32_t num;
  int32_t den;
};

static inline struct WaylandScale waylandScaleFromInt(int32_t scale)
{
  return (struct WaylandScale) { scale, 1 };
}

static inline struct WaylandScale waylandScaleFromRatio(int32_t num, int32_t den)
{
  return (struct WaylandScale) { num, den };
}

static inline bool waylandScaleValid(struct WaylandScale scale)
{
  return scale.num > 0 && scale.den > 0;
}

static inline bool waylandScaleEqual(struct WaylandScale a, struct WaylandScale b)
{
  return (int64_t)a.num * b.den == (int64_t)b.num * a.den;
}

static inline int waylandScaleCmp(struct WaylandScale a, struct WaylandScale b)
{
  int64_t lhs = (int64_t)a.num * b.den;
  int64_t rhs = (int64_t)b.num * a.den;

  return (lhs > rhs) - (lhs < rhs);
}

static inline bool waylandScaleIsFractional(struct WaylandScale scale)
{
  return waylandScaleValid(scale) && scale.num % scale.den != 0;
}

static inline int waylandScaleFloor(struct WaylandScale scale)
{
  return scale.num / scale.den;
}

static inline int waylandScaleCeil(struct WaylandScale scale)
{
  return (scale.num + scale.den - 1) / scale.den;
}

static inline int waylandScaleMulInt(struct WaylandScale scale, int value)
{
  return (int)(((int64_t)value * scale.num + scale.den / 2) / scale.den);
}

static inline double waylandScaleToDouble(struct WaylandScale scale)
{
  return (double)scale.num / (double)scale.den;
}
