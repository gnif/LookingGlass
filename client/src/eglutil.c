/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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

#include "eglutil.h"

#include "common/debug.h"
#include "egl_dynprocs.h"
#include "util.h"

void swapWithDamageInit(struct SwapWithDamageData * data, EGLDisplay display)
{
  const char *exts = eglQueryString(display, EGL_EXTENSIONS);
  data->init = true;

  if (util_hasGLExt(exts, "EGL_KHR_swap_buffers_with_damage") && g_egl_dynProcs.eglSwapBuffersWithDamageKHR)
  {
    data->func = g_egl_dynProcs.eglSwapBuffersWithDamageKHR;
    DEBUG_INFO("Using EGL_KHR_swap_buffers_with_damage");
  }
  else if (util_hasGLExt(exts, "EGL_EXT_swap_buffers_with_damage") && g_egl_dynProcs.eglSwapBuffersWithDamageEXT)
  {
    data->func = g_egl_dynProcs.eglSwapBuffersWithDamageEXT;
    DEBUG_INFO("Using EGL_EXT_swap_buffers_with_damage");
  }
  else
  {
    data->func = NULL;
    DEBUG_INFO("Swapping buffers with damage: not supported");
  }
}

void swapWithDamageDisable(struct SwapWithDamageData * data)
{
  data->init = false;
  data->func = NULL;
}

void swapWithDamage(struct SwapWithDamageData * data, EGLDisplay display, EGLSurface surface,
    const struct Rect * damage, int count)
{
  if (!data->func || !count)
  {
    eglSwapBuffers(display, surface);
    return;
  }

  EGLint rects[count * 4];
  for (int i = 0; i < count; ++i)
  {
    rects[i * 4 + 0] = damage[i].x;
    rects[i * 4 + 1] = damage[i].y;
    rects[i * 4 + 2] = damage[i].w;
    rects[i * 4 + 3] = damage[i].h;
  }
  data->func(display, surface, rects, count);
}
