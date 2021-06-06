/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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

#include "wayland.h"

#include <stdbool.h>
#include <string.h>

#include <EGL/egl.h>
#include <wayland-client.h>

#include "app.h"
#include "common/debug.h"
#include "util.h"

#if defined(ENABLE_EGL) || defined(ENABLE_OPENGL)
#include "egl_dynprocs.h"

bool waylandEGLInit(int w, int h)
{
  wlWm.eglWindow = wl_egl_window_create(wlWm.surface, w, h);
  if (!wlWm.eglWindow)
  {
    DEBUG_ERROR("Failed to create EGL window");
    return false;
  }

  return true;
}

EGLDisplay waylandGetEGLDisplay(void)
{
  EGLNativeDisplayType native = (EGLNativeDisplayType) wlWm.display;

  const char *early_exts = eglQueryString(NULL, EGL_EXTENSIONS);

  if (util_hasGLExt(early_exts, "EGL_KHR_platform_wayland") &&
      g_egl_dynProcs.eglGetPlatformDisplay)
  {
    DEBUG_INFO("Using eglGetPlatformDisplay");
    return g_egl_dynProcs.eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, native, NULL);
  }

  if (util_hasGLExt(early_exts, "EGL_EXT_platform_wayland") &&
      g_egl_dynProcs.eglGetPlatformDisplayEXT)
  {
    DEBUG_INFO("Using eglGetPlatformDisplayEXT");
    return g_egl_dynProcs.eglGetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_EXT, native, NULL);
  }

  DEBUG_INFO("Using eglGetDisplay");
  return eglGetDisplay(native);
}

void waylandEGLSwapBuffers(EGLDisplay display, EGLSurface surface, const struct Rect * damage, int count)
{
  if (!wlWm.eglSwapWithDamageInit)
  {
    const char *exts = eglQueryString(display, EGL_EXTENSIONS);
    wlWm.eglSwapWithDamageInit = true;
    if (wl_proxy_get_version((struct wl_proxy *) wlWm.surface) < 4)
      DEBUG_INFO("Swapping buffers with damage: not supported, need wl_compositor v4");
    else if (util_hasGLExt(exts, "EGL_KHR_swap_buffers_with_damage") && g_egl_dynProcs.eglSwapBuffersWithDamageKHR)
    {
      wlWm.eglSwapWithDamage = g_egl_dynProcs.eglSwapBuffersWithDamageKHR;
      DEBUG_INFO("Using EGL_KHR_swap_buffers_with_damage");
    }
    else if (util_hasGLExt(exts, "EGL_EXT_swap_buffers_with_damage") && g_egl_dynProcs.eglSwapBuffersWithDamageEXT)
    {
      wlWm.eglSwapWithDamage = g_egl_dynProcs.eglSwapBuffersWithDamageEXT;
      DEBUG_INFO("Using EGL_EXT_swap_buffers_with_damage");
    }
    else
      DEBUG_INFO("Swapping buffers with damage: not supported");
  }

  if (wlWm.eglSwapWithDamage && count)
  {
    if (count * 4 > wlWm.eglDamageRectCount)
    {
      free(wlWm.eglDamageRects);
      wlWm.eglDamageRects = malloc(sizeof(EGLint) * count * 4);
      if (!wlWm.eglDamageRects)
        DEBUG_FATAL("Out of memory");
      wlWm.eglDamageRectCount = count * 4;
    }

    for (int i = 0; i < count; ++i)
    {
      wlWm.eglDamageRects[i*4+0] = damage[i].x;
      wlWm.eglDamageRects[i*4+1] = damage[i].y;
      wlWm.eglDamageRects[i*4+2] = damage[i].w;
      wlWm.eglDamageRects[i*4+3] = damage[i].h;
    }

    wlWm.eglSwapWithDamage(display, surface, wlWm.eglDamageRects, count);
  }
  else
    eglSwapBuffers(display, surface);

  if (wlWm.needsResize)
  {
    wl_egl_window_resize(wlWm.eglWindow, wlWm.width * wlWm.scale, wlWm.height * wlWm.scale, 0, 0);
    wl_surface_set_buffer_scale(wlWm.surface, wlWm.scale);

    struct wl_region * region = wl_compositor_create_region(wlWm.compositor);
    wl_region_add(region, 0, 0, wlWm.width, wlWm.height);
    wl_surface_set_opaque_region(wlWm.surface, region);
    wl_region_destroy(region);

    app_handleResizeEvent(wlWm.width, wlWm.height, wlWm.scale, (struct Border) {0, 0, 0, 0});
    wlWm.needsResize = false;
  }

  waylandShellAckConfigureIfNeeded();
}
#endif

#ifdef ENABLE_EGL
EGLNativeWindowType waylandGetEGLNativeWindow(void)
{
  return (EGLNativeWindowType) wlWm.eglWindow;
}
#endif

#ifdef ENABLE_OPENGL
bool waylandOpenGLInit(void)
{
  EGLint attr[] =
  {
    EGL_BUFFER_SIZE      , 24,
    EGL_CONFORMANT       , EGL_OPENGL_BIT,
    EGL_RENDERABLE_TYPE  , EGL_OPENGL_BIT,
    EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER,
    EGL_RED_SIZE         , 8,
    EGL_GREEN_SIZE       , 8,
    EGL_BLUE_SIZE        , 8,
    EGL_SAMPLE_BUFFERS   , 0,
    EGL_SAMPLES          , 0,
    EGL_NONE
  };

  wlWm.glDisplay = waylandGetEGLDisplay();

  int maj, min;
  if (!eglInitialize(wlWm.glDisplay, &maj, &min))
  {
    DEBUG_ERROR("Unable to initialize EGL");
    return false;
  }

  if (wlWm.glDisplay == EGL_NO_DISPLAY)
  {
    DEBUG_ERROR("Failed to get EGL display (eglError: 0x%x)", eglGetError());
    return false;
  }

  EGLint num_config;
  if (!eglChooseConfig(wlWm.glDisplay, attr, &wlWm.glConfig, 1, &num_config))
  {
    DEBUG_ERROR("Failed to choose config (eglError: 0x%x)", eglGetError());
    return false;
  }

  wlWm.glSurface = eglCreateWindowSurface(wlWm.glDisplay, wlWm.glConfig, wlWm.eglWindow, NULL);
  if (wlWm.glSurface == EGL_NO_SURFACE)
  {
    DEBUG_ERROR("Failed to create EGL surface (eglError: 0x%x)", eglGetError());
    return false;
  }

  return true;
}

LG_DSGLContext waylandGLCreateContext(void)
{
  eglBindAPI(EGL_OPENGL_API);
  return eglCreateContext(wlWm.glDisplay, wlWm.glConfig, EGL_NO_CONTEXT, NULL);
}

void waylandGLDeleteContext(LG_DSGLContext context)
{
  eglDestroyContext(wlWm.glDisplay, context);
}

void waylandGLMakeCurrent(LG_DSGLContext context)
{
  eglMakeCurrent(wlWm.glDisplay, wlWm.glSurface, wlWm.glSurface, context);
}

void waylandGLSetSwapInterval(int interval)
{
  eglSwapInterval(wlWm.glDisplay, interval);
}

void waylandGLSwapBuffers(void)
{
  waylandEGLSwapBuffers(wlWm.glDisplay, wlWm.glSurface, NULL, 0);
}
#endif
