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

#ifndef _H_LG_GLUTIL_
#define _H_LG_GLUTIL_

#include <stdbool.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "common/types.h"

struct SwapWithDamageData
{
  bool init;
  PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC func;
};

void swapWithDamageInit(struct SwapWithDamageData * data, EGLDisplay display);
void swapWithDamageDisable(struct SwapWithDamageData * data);
void swapWithDamage(struct SwapWithDamageData * data, EGLDisplay display, EGLSurface surface,
    const struct Rect * damage, int count);

#endif
