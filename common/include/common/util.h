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

#ifndef _H_LG_COMMON_UTIL_
#define _H_LG_COMMON_UTIL_

#include <stddef.h>

#ifndef min
#define min(a,b) ({ __typeof__ (a) _a = (a); __typeof__ (b) _b = (b); \
    _a < _b ? _a : _b; })
#endif

#ifndef max
#define max(a,b) ({ __typeof__ (a) _a = (a); __typeof__ (b) _b = (b); \
    _a > _b ? _a : _b; })
#endif

#ifndef clamp
#define clamp(v,a,b) min(max(v, a), b)
#endif

#define UPCAST(type, x) \
  (type *)((uintptr_t)(x) - offsetof(type, base))

#define ALIGN_TO(value, align) (((value) + (align) - 1) & -(align))

#define unlikely(expr) __builtin_expect(!!(expr), 0)
#define likely(expr)   __builtin_expect(!!(expr), 1)

#define _STR(x) #x
#define STR(x) _STR(x)

#endif
