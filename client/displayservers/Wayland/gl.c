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
#include "eglutil.h"

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

static void applyHDRPending(void)
{
  if (wlWm.pendingHDRApply)
  {
    wlWm.pendingHDRApply = false;
    atomic_thread_fence(memory_order_acquire);
    waylandSetHDRImageDescription(
        wlWm.pendingHDRDisplayPrimary, wlWm.pendingHDRWhitePoint,
        wlWm.pendingHDRMaxDisplayLuminance,
        wlWm.pendingHDRMinDisplayLuminance,
        wlWm.pendingHDRMaxCLL,
        wlWm.pendingHDRMaxFALL,
        wlWm.pendingHDRPQ);
  }
  else if (wlWm.pendingHDRClear)
  {
    wlWm.pendingHDRClear = false;
    waylandClearHDRImageDescription();
  }
}

void waylandClearHDRImageDescription(void)
{
  if (!wlWm.hdrActive)
    return;

  if (wlWm.hdrImageCreator)
  {
    wp_image_description_creator_params_v1_destroy(wlWm.hdrImageCreator);
    wlWm.hdrImageCreator = NULL;
  }
  if (wlWm.hdrImageDesc)
  {
    wp_image_description_v1_destroy(wlWm.hdrImageDesc);
    wlWm.hdrImageDesc = NULL;
  }
  if (wlWm.colorSurface)
  {
    wp_color_management_surface_v1_destroy(wlWm.colorSurface);
    wlWm.colorSurface = NULL;
  }

  wlWm.hdrActive = false;
  DEBUG_INFO("HDR image description cleared");
}

void waylandEGLSwapBuffers(EGLDisplay display, EGLSurface surface, const struct Rect * damage, int count)
{
  if (!wlWm.swapWithDamage.init)
  {
    if (wl_proxy_get_version((struct wl_proxy *) wlWm.surface) < 4)
    {
      DEBUG_INFO("Swapping buffers with damage: not supported, need wl_compositor v4");
      swapWithDamageDisable(&wlWm.swapWithDamage);
    }
    else
      swapWithDamageInit(&wlWm.swapWithDamage, display);
  }

  waylandPresentationFrame();
  applyHDRPending();
  swapWithDamage(&wlWm.swapWithDamage, display, surface, damage, count);

  if (wlWm.needsResize)
  {
    bool skipResize = false;

    int width, height;
    wlWm.desktop->getSize(&width, &height);
    wl_egl_window_resize(wlWm.eglWindow, waylandScaleMulInt(wlWm.scale, width),
        waylandScaleMulInt(wlWm.scale, height), 0, 0);

    if (width == 0 || height == 0)
      skipResize = true;
    else if (wlWm.fractionalScale)
    {
      wl_surface_set_buffer_scale(wlWm.surface, 1);
      if (!wlWm.viewport)
        wlWm.viewport = wp_viewporter_get_viewport(wlWm.viewporter, wlWm.surface);
      wp_viewport_set_source(
          wlWm.viewport,
          wl_fixed_from_int(-1), wl_fixed_from_int(-1),
          wl_fixed_from_int(-1), wl_fixed_from_int(-1)
      );
      wp_viewport_set_destination(wlWm.viewport, width, height);
    }
    else
    {
      if (wlWm.viewport)
      {
        // Clearing the source and destination rectangles should happen in wp_viewport_destroy.
        // However, wlroots does not clear the rectangle until fixed in 456c6e22 (2021-08-02).
        // This should be kept to work around old versions of wlroots.
        wl_fixed_t clear = wl_fixed_from_int(-1);
        wp_viewport_set_source(wlWm.viewport, clear, clear, clear, clear);
        wp_viewport_set_destination(wlWm.viewport, -1, -1);

        wp_viewport_destroy(wlWm.viewport);
        wlWm.viewport = NULL;
      }
      wl_surface_set_buffer_scale(wlWm.surface, waylandScaleFloor(wlWm.scale));
    }

    struct wl_region * region = wl_compositor_create_region(wlWm.compositor);
    wl_region_add(region, 0, 0, width, height);
    wl_surface_set_opaque_region(wlWm.surface, region);
    wl_region_destroy(region);

    app_handleResizeEvent(width, height, waylandScaleToDouble(wlWm.scale),
        (struct Border) {0, 0, 0, 0});
    app_invalidateWindow(true);
    waylandStopWaitFrame();
    wlWm.needsResize = skipResize;
  }

  wlWm.desktop->shellAckConfigureIfNeeded();
}
#endif

#ifdef ENABLE_EGL
EGLNativeWindowType waylandGetEGLNativeWindow(void)
{
  return (EGLNativeWindowType) wlWm.eglWindow;
}
#endif

// HDR color management support
void waylandSetHDRImageDescription(const uint16_t displayPrimary[3][2],
    const uint16_t whitePoint[2], uint32_t maxDisplayLuminance,
    uint32_t minDisplayLuminance, uint32_t maxCLL, uint32_t maxFALL,
    bool hdrPQ)
{
  if (!wlWm.colorManager)
    return;

  if (!wlWm.cmFeaturesDone)
  {
    DEBUG_WARN("Color management features not yet advertised, deferring HDR");
    return;
  }

  if (!wlWm.cmHasParametric)
  {
    DEBUG_WARN("Compositor does not support parametric image descriptions");
    return;
  }

  // Verify the compositor supports the transfer function we need
  if (hdrPQ && !wlWm.cmHasTFSt2084PQ)
  {
    DEBUG_WARN("Compositor does not support ST2084_PQ transfer function");
    return;
  }
  if (!hdrPQ && !wlWm.cmHasTFExtLinear)
  {
    DEBUG_WARN("Compositor does not support EXT_LINEAR transfer function");
    return;
  }

  // Verify primaries support for the target color space
  if (hdrPQ && !wlWm.cmHasPrimariesBT2020)
  {
    DEBUG_WARN("Compositor does not support BT.2020 primaries");
    return;
  }
  if (!hdrPQ && !wlWm.cmHasPrimariesSRGB)
  {
    DEBUG_WARN("Compositor does not support sRGB primaries");
    return;
  }

  // Clean up any previous description (allows re-application with updated metadata)
  if (wlWm.hdrImageCreator)
  {
    wp_image_description_creator_params_v1_destroy(wlWm.hdrImageCreator);
    wlWm.hdrImageCreator = NULL;
  }
  if (wlWm.hdrImageDesc)
  {
    wp_image_description_v1_destroy(wlWm.hdrImageDesc);
    wlWm.hdrImageDesc = NULL;
  }
  if (wlWm.colorSurface)
  {
    wp_color_management_surface_v1_destroy(wlWm.colorSurface);
    wlWm.colorSurface = NULL;
  }

  wlWm.hdrActive = false;

  wlWm.hdrImageCreator =
    wp_color_manager_v1_create_parametric_creator(wlWm.colorManager);
  if (!wlWm.hdrImageCreator)
  {
    DEBUG_WARN("Failed to create parametric image description creator");
    return;
  }

  // Set primaries: BT.2020 for PQ HDR10, sRGB for scRGB/FP16
  wp_image_description_creator_params_v1_set_primaries_named(
      wlWm.hdrImageCreator,
      hdrPQ ? WP_COLOR_MANAGER_V1_PRIMARIES_BT2020
            : WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);

  // Select transfer function: PQ for PQ-encoded content, linear for scRGB/FP16
  wp_image_description_creator_params_v1_set_tf_named(
      wlWm.hdrImageCreator,
      hdrPQ ? WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ
            : WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR);

  // Set luminance range from metadata.
  // min_lum is in 0.0001 cd/m² units (multiplied by 10000 as per protocol).
  // max_lum and reference_lum are in unscaled cd/m² units.
  if (wlWm.cmHasLuminances)
    wp_image_description_creator_params_v1_set_luminances(
        wlWm.hdrImageCreator,
        minDisplayLuminance > 0 ? minDisplayLuminance : 50,
        maxDisplayLuminance > 0 ? maxDisplayLuminance / 10000 : 1000,
        hdrPQ                   ? 203                         : 80);

  // Set mastering display primaries from frame HDR metadata.
  // Always set when the compositor supports it, falling back to the
  // primaries from the metadata (even if zero, in which case the
  // compositor uses its own defaults).
  if (wlWm.cmHasMasteringPrimaries)
  {
    wp_image_description_creator_params_v1_set_mastering_display_primaries(
        wlWm.hdrImageCreator,
        displayPrimary[0][0], displayPrimary[0][1],
        displayPrimary[1][0], displayPrimary[1][1],
        displayPrimary[2][0], displayPrimary[2][1],
        whitePoint[0], whitePoint[1]);

    // Set mastering luminance range so the compositor knows the target
    // color volume for tone mapping.  min_lum is in ×10000 units (0.0001 cd/m²),
    // max_lum is in unscaled cd/m².
    wp_image_description_creator_params_v1_set_mastering_luminance(
        wlWm.hdrImageCreator,
        minDisplayLuminance > 0 ? minDisplayLuminance : 50,
        maxDisplayLuminance > 0 ? maxDisplayLuminance / 10000 : 1000);
  }

  if (maxCLL > 0)
    wp_image_description_creator_params_v1_set_max_cll(wlWm.hdrImageCreator, maxCLL);
  if (maxFALL > 0)
    wp_image_description_creator_params_v1_set_max_fall(wlWm.hdrImageCreator, maxFALL);

  wlWm.hdrImageDesc =
    wp_image_description_creator_params_v1_create(wlWm.hdrImageCreator);
  wlWm.hdrImageCreator = NULL; // consumed by create

  if (!wlWm.hdrImageDesc)
  {
    DEBUG_WARN("Failed to create HDR image description");
    return;
  }

  wlWm.colorSurface =
    wp_color_manager_v1_get_surface(wlWm.colorManager, wlWm.surface);
  if (!wlWm.colorSurface)
  {
    DEBUG_WARN("Failed to get color management surface");
    wp_image_description_v1_destroy(wlWm.hdrImageDesc);
    wlWm.hdrImageDesc = NULL;
    return;
  }

  wp_color_management_surface_v1_set_image_description(
      wlWm.colorSurface, wlWm.hdrImageDesc,
      WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);

  wlWm.hdrActive = true;
  DEBUG_INFO("HDR image description set on surface (%s, %s, %u nits)",
      hdrPQ ? "PQ" : "scRGB", hdrPQ ? "BT.2020" : "sRGB", maxDisplayLuminance);
}

bool waylandRequestHDR(const uint16_t displayPrimary[3][2],
    const uint16_t whitePoint[2], uint32_t maxDisplayLuminance,
    uint32_t minDisplayLuminance, uint32_t maxCLL, uint32_t maxFALL,
    bool hdrPQ)
{
  if (!wlWm.cmFeaturesDone || !wlWm.cmHasParametric)
    return false;
  if (hdrPQ && !wlWm.cmHasTFSt2084PQ)
    return false;
  if (!hdrPQ && !wlWm.cmHasTFExtLinear)
    return false;
  if (hdrPQ && !wlWm.cmHasPrimariesBT2020)
    return false;
  if (!hdrPQ && !wlWm.cmHasPrimariesSRGB)
    return false;

  wlWm.pendingHDRPQ = hdrPQ;

  // Write all metadata before setting the apply flag to ensure the
  // render thread sees the complete HDR metadata when it observes
  // pendingHDRApply == true.
  memcpy(wlWm.pendingHDRDisplayPrimary, displayPrimary,
      sizeof(wlWm.pendingHDRDisplayPrimary));
  memcpy(wlWm.pendingHDRWhitePoint, whitePoint,
      sizeof(wlWm.pendingHDRWhitePoint));
  wlWm.pendingHDRMaxDisplayLuminance = maxDisplayLuminance;
  wlWm.pendingHDRMinDisplayLuminance = minDisplayLuminance;
  wlWm.pendingHDRMaxCLL              = maxCLL;
  wlWm.pendingHDRMaxFALL             = maxFALL;

  wlWm.pendingHDRClear = false;
  atomic_thread_fence(memory_order_release);
  wlWm.pendingHDRApply = true;
  return true;
}

void waylandRequestClearHDR(void)
{
  wlWm.pendingHDRClear = true;
  wlWm.pendingHDRApply = false;
}

#ifdef ENABLE_OPENGL

static const EGLint eglBaseAttrs[] =
{
  EGL_CONFORMANT       , EGL_OPENGL_BIT,
  EGL_RENDERABLE_TYPE  , EGL_OPENGL_BIT,
  EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER,
  EGL_SAMPLE_BUFFERS   , 0,
  EGL_SAMPLES          , 0,
  EGL_NONE
};

static bool eglChooseConfigWithDepth(EGLDisplay display, EGLint red,
    EGLint green, EGLint blue, EGLint alpha, EGLint depth,
    EGLConfig * config, const char ** desc)
{
  // Build attr array with the base attrs plus color depth
  EGLint attr[32];
  int   ai = 0;
  attr[ai++] = EGL_RED_SIZE  ; attr[ai++] = red;
  attr[ai++] = EGL_GREEN_SIZE; attr[ai++] = green;
  attr[ai++] = EGL_BLUE_SIZE ; attr[ai++] = blue;
  attr[ai++] = EGL_ALPHA_SIZE; attr[ai++] = alpha;
  attr[ai++] = EGL_BUFFER_SIZE; attr[ai++] = depth;
  for (int i = 0; eglBaseAttrs[i] != EGL_NONE; ++i)
    attr[ai++] = eglBaseAttrs[i];
  attr[ai] = EGL_NONE;

  EGLint num_config;
  if (eglChooseConfig(display, attr, config, 1, &num_config) && num_config > 0)
  {
    if (desc)
      *desc = red >= 16 ? "FP16 (RGBA16F)" :
              red >= 10 ? "10-bit (RGBA10)" : "8-bit (RGBA8)";
    return true;
  }
  return false;
}

bool waylandOpenGLInit(void)
{
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

  EGLConfig config;
  const char * configDesc = NULL;

  // Probe for best available color depth: FP16 → 10-bit → 8-bit
  if (!eglChooseConfigWithDepth(wlWm.glDisplay,
        16, 16, 16, 0, 48, &config, &configDesc) &&
      !eglChooseConfigWithDepth(wlWm.glDisplay,
        10, 10, 10, 2, 32, &config, &configDesc) &&
      !eglChooseConfigWithDepth(wlWm.glDisplay,
        8, 8, 8, 0, 24, &config, &configDesc))
  {
    DEBUG_ERROR("Failed to choose any EGL config");
    return false;
  }

  wlWm.glConfig = config;
  DEBUG_INFO("EGL config: %s", configDesc);

  // Also store the 8-bit fallback for SDR contexts
  if (configDesc && strcmp(configDesc, "8-bit (RGBA8)") != 0)
    eglChooseConfigWithDepth(wlWm.glDisplay,
        8, 8, 8, 0, 24, &wlWm.glConfigSDR, NULL);
  else
    wlWm.glConfigSDR = wlWm.glConfig;

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
