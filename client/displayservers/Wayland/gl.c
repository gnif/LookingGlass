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
  enum WaylandHDRPendingAction action;
  struct WaylandHDRParameters params;

  LG_LOCK(wlWm.pendingHDRLock);
  action = wlWm.pendingHDRAction;
  params = wlWm.pendingHDR;
  wlWm.pendingHDRAction = WAYLAND_HDR_PENDING_NONE;
  LG_UNLOCK(wlWm.pendingHDRLock);

  if (action == WAYLAND_HDR_PENDING_APPLY)
  {
    waylandSetHDRImageDescription(
        params.displayPrimary, params.whitePoint,
        params.maxDisplayLuminance,
        params.minDisplayLuminance,
        params.maxCLL,
        params.maxFALL,
        params.pq,
        params.metadata);
  }
  else if (action == WAYLAND_HDR_PENDING_CLEAR)
    waylandClearHDRImageDescription();
}

void waylandClearHDRImageDescription(void)
{
  LG_LOCK(wlWm.hdrLock);
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
  wlWm.hdrImageDescReady = false;

  if (wlWm.colorSurface && atomic_load(&wlWm.hdrActive))
    wp_color_management_surface_v1_unset_image_description(wlWm.colorSurface);

  atomic_store(&wlWm.hdrActive, false);
  LG_UNLOCK(wlWm.hdrLock);
  DEBUG_INFO("HDR image description removed from surface");
}

static void hdrImageDescriptionReady(struct wp_image_description_v1 * desc)
{
  LG_LOCK(wlWm.hdrLock);
  if (desc != wlWm.hdrImageDesc)
  {
    LG_UNLOCK(wlWm.hdrLock);
    return;
  }

  if (!wlWm.colorSurface)
  {
    DEBUG_WARN("HDR image description became ready without a color surface");
    wp_image_description_v1_destroy(desc);
    wlWm.hdrImageDesc = NULL;
    LG_UNLOCK(wlWm.hdrLock);
    return;
  }

  // Defer attachment until just after the current surface commit. EGL can
  // then switch to native HDR for the next render, and that native frame and
  // its image description become active in the same following commit.
  wlWm.hdrImageDescReady = true;
  const bool pq = wlWm.hdrImageDescPQ;
  LG_UNLOCK(wlWm.hdrLock);
  DEBUG_INFO("HDR image description is ready (%s)",
      pq ? "PQ" : "scRGB");
  app_invalidateWindow(true);
  waylandStopWaitFrame();
}

static void activateReadyHDRImageDescription(void)
{
  LG_LOCK(wlWm.hdrLock);
  if (!wlWm.hdrImageDesc || !wlWm.hdrImageDescReady || !wlWm.colorSurface)
  {
    LG_UNLOCK(wlWm.hdrLock);
    return;
  }

  struct wp_image_description_v1 * desc = wlWm.hdrImageDesc;
  wp_color_management_surface_v1_set_image_description(
      wlWm.colorSurface, desc,
      WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
  if (wlWm.hdrImageDescPQ)
    atomic_store(&wlWm.hdrActivePQWhiteLevel,
        wlWm.hdrImageDescWhiteLevel);
  atomic_store(&wlWm.hdrActivePQ, wlWm.hdrImageDescPQ);
  atomic_store(&wlWm.hdrActive, true);

  // set_image_description has copy semantics; the protocol object is no
  // longer needed once it has been attached to the pending surface state.
  wlWm.hdrImageDesc = NULL;
  wlWm.hdrImageDescReady = false;
  wp_image_description_v1_destroy(desc);
  const bool pq = atomic_load(&wlWm.hdrActivePQ);
  LG_UNLOCK(wlWm.hdrLock);

  DEBUG_INFO("HDR image description pending next surface commit (%s)",
      pq ? "PQ" : "scRGB");
  app_invalidateWindow(true);
  waylandStopWaitFrame();
}

static void hdrImageDescriptionFailed(void * data,
    struct wp_image_description_v1 * desc, uint32_t cause, const char * message)
{
  (void)data;
  LG_LOCK(wlWm.hdrLock);
  if (desc != wlWm.hdrImageDesc)
  {
    LG_UNLOCK(wlWm.hdrLock);
    return;
  }

  DEBUG_WARN("Failed to create HDR image description (cause:%u): %s",
      cause, message);
  if (atomic_load(&wlWm.hdrActive))
    DEBUG_WARN("Retaining the previous active HDR image description");
  wlWm.hdrImageDesc = NULL;
  wlWm.hdrImageDescReady = false;
  wp_image_description_v1_destroy(desc);
  LG_UNLOCK(wlWm.hdrLock);
  app_invalidateWindow(true);
  waylandStopWaitFrame();
}

static void hdrImageDescriptionReadyV1(void * data,
    struct wp_image_description_v1 * desc, uint32_t identity)
{
  (void)data;
  (void)identity;
  hdrImageDescriptionReady(desc);
}

static void hdrImageDescriptionReadyV2(void * data,
    struct wp_image_description_v1 * desc, uint32_t identityHi,
    uint32_t identityLo)
{
  (void)data;
  (void)identityHi;
  (void)identityLo;
  hdrImageDescriptionReady(desc);
}

static const struct wp_image_description_v1_listener hdrImageDescListener =
{
  .failed = hdrImageDescriptionFailed,
  .ready  = hdrImageDescriptionReadyV1,
  .ready2 = hdrImageDescriptionReadyV2,
};

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
  activateReadyHDRImageDescription();

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
enum
{
  HDR_PQ_MIN_LUMINANCE       = 50,
  HDR_PQ_MAX_LUMINANCE       = 10000,
  HDR_PQ_DEFAULT_WHITE_LEVEL = 203,
};

static int64_t hdrTriangleEdge(int32_t ax, int32_t ay,
    int32_t bx, int32_t by, int32_t px, int32_t py)
{
  return (int64_t)(px - ax) * (by - ay) -
    (int64_t)(py - ay) * (bx - ax);
}

static bool hdrPointInBT2020(int32_t x, int32_t y)
{
  // BT.2020 primaries in the KVMFR/DXGI scale of 50,000 units.
  static const int32_t primary[3][2] =
  {
    { 35400, 14600 },
    {  8500, 39850 },
    {  6550,  2300 },
  };

  const int64_t edge0 = hdrTriangleEdge(
      primary[0][0], primary[0][1], primary[1][0], primary[1][1], x, y);
  const int64_t edge1 = hdrTriangleEdge(
      primary[1][0], primary[1][1], primary[2][0], primary[2][1], x, y);
  const int64_t edge2 = hdrTriangleEdge(
      primary[2][0], primary[2][1], primary[0][0], primary[0][1], x, y);
  return (edge0 >= 0 && edge1 >= 0 && edge2 >= 0) ||
    (edge0 <= 0 && edge1 <= 0 && edge2 <= 0);
}

static bool hdrChromaticitiesValid(const uint16_t primary[3][2],
    const uint16_t whitePoint[2])
{
  for (unsigned i = 0; i < 3; ++i)
    if (primary[i][1] == 0 || primary[i][0] > 50000 ||
        primary[i][1] > 50000 ||
        (uint32_t)primary[i][0] + primary[i][1] > 50000)
      return false;

  if (whitePoint[1] == 0 || whitePoint[0] > 50000 ||
      whitePoint[1] > 50000 ||
      (uint32_t)whitePoint[0] + whitePoint[1] > 50000)
    return false;

  return hdrTriangleEdge(
      primary[0][0], primary[0][1], primary[1][0], primary[1][1],
      primary[2][0], primary[2][1]) != 0;
}

static bool hdrTargetVolumeContained(const uint16_t primary[3][2],
    const uint16_t whitePoint[2], uint32_t minLuminance,
    uint32_t maxLuminance)
{
  for (unsigned i = 0; i < 3; ++i)
    if (!hdrPointInBT2020(primary[i][0], primary[i][1]))
      return false;

  return hdrPointInBT2020(whitePoint[0], whitePoint[1]) &&
    minLuminance >= HDR_PQ_MIN_LUMINANCE &&
    maxLuminance <= HDR_PQ_MAX_LUMINANCE;
}

void waylandSetHDRImageDescription(const uint16_t displayPrimary[3][2],
    const uint16_t whitePoint[2], uint32_t maxDisplayLuminance,
    uint32_t minDisplayLuminance, uint32_t maxCLL, uint32_t maxFALL,
    bool hdrPQ, bool hdrMetadata)
{
  if (!wlWm.colorManager)
    return;

  if (!atomic_load_explicit(&wlWm.cmFeaturesDone, memory_order_acquire))
  {
    DEBUG_WARN("Color management features not yet advertised, deferring HDR");
    return;
  }

  if (!wlWm.cmHasPerceptualIntent)
  {
    DEBUG_WARN("Compositor does not support the perceptual render intent");
    return;
  }

  if (hdrPQ && !wlWm.cmHasParametric)
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
  if (!hdrPQ && !wlWm.cmHasWindowsSCRGB)
  {
    DEBUG_WARN("Compositor does not support Windows-scRGB image descriptions");
    return;
  }

  // Verify primaries support for the target color space
  if (hdrPQ && !wlWm.cmHasPrimariesBT2020)
  {
    DEBUG_WARN("Compositor does not support BT.2020 primaries");
    return;
  }
  LG_LOCK(wlWm.hdrLock);

  // Cancel only an in-flight replacement. The active surface description is
  // retained until this replacement is ready.
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
  wlWm.hdrImageDescReady = false;
  // If the encoding changed, remove the old description in the same surface
  // commit that presents the software-mapped transition frame.
  if (atomic_load(&wlWm.hdrActive) &&
      atomic_load(&wlWm.hdrActivePQ) != hdrPQ)
  {
    wp_color_management_surface_v1_unset_image_description(wlWm.colorSurface);
    atomic_store(&wlWm.hdrActive, false);
  }

  if (!wlWm.colorSurface)
  {
    wlWm.colorSurface =
      wp_color_manager_v1_get_surface(wlWm.colorManager, wlWm.surface);
    if (!wlWm.colorSurface)
    {
      DEBUG_WARN("Failed to get color management surface");
      LG_UNLOCK(wlWm.hdrLock);
      return;
    }
  }

  if (!hdrPQ)
  {
    wlWm.hdrImageDesc =
      wp_color_manager_v1_create_windows_scrgb(wlWm.colorManager);
    if (!wlWm.hdrImageDesc)
    {
      DEBUG_WARN("Failed to create Windows-scRGB image description");
      LG_UNLOCK(wlWm.hdrLock);
      return;
    }

    wlWm.hdrImageDescPQ = false;
    wlWm.hdrImageDescReady = false;
    wp_image_description_v1_add_listener(
        wlWm.hdrImageDesc, &hdrImageDescListener, NULL);
    LG_UNLOCK(wlWm.hdrLock);
    DEBUG_INFO("HDR image description requested (scRGB, Windows-scRGB)");
    return;
  }

  wlWm.hdrImageCreator =
    wp_color_manager_v1_create_parametric_creator(wlWm.colorManager);
  if (!wlWm.hdrImageCreator)
  {
    DEBUG_WARN("Failed to create parametric image description creator");
    LG_UNLOCK(wlWm.hdrLock);
    return;
  }

  // Set primaries: BT.2020 for PQ HDR10, sRGB for scRGB/FP16
  wp_image_description_creator_params_v1_set_primaries_named(
      wlWm.hdrImageCreator,
      WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);

  // Select transfer function: PQ for PQ-encoded content, linear for scRGB/FP16
  wp_image_description_creator_params_v1_set_tf_named(
      wlWm.hdrImageCreator,
      WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);

  wlWm.hdrImageDescWhiteLevel = wlWm.cmHasLuminances ?
    atomic_load(&wlWm.hdrPQWhiteLevel) : HDR_PQ_DEFAULT_WHITE_LEVEL;

  // The primary colour volume describes the PQ encoding itself. Mastering
  // display luminances are target-volume metadata and are set separately.
  if (wlWm.cmHasLuminances)
    wp_image_description_creator_params_v1_set_luminances(
        wlWm.hdrImageCreator,
        HDR_PQ_MIN_LUMINANCE,
        HDR_PQ_MAX_LUMINANCE,
        wlWm.hdrImageDescWhiteLevel);

  // KVMFR uses the ST 2086/DXGI scale of 50,000 units per coordinate while
  // color-management-v1 uses 1,000,000 units per coordinate.
  const bool validMasteringLuminance =
    (uint64_t)maxDisplayLuminance * 10000 > minDisplayLuminance;
  const bool validMasteringChromaticities =
    hdrChromaticitiesValid(displayPrimary, whitePoint);
  const bool targetVolumeContained = hdrTargetVolumeContained(
      displayPrimary, whitePoint, minDisplayLuminance,
      maxDisplayLuminance);
  const bool canSetMasteringMetadata = hdrPQ && hdrMetadata &&
    wlWm.cmHasMasteringPrimaries && validMasteringLuminance &&
    validMasteringChromaticities &&
    (targetVolumeContained || wlWm.cmHasExtendedTargetVolume);
  if (canSetMasteringMetadata)
  {
    wp_image_description_creator_params_v1_set_mastering_display_primaries(
        wlWm.hdrImageCreator,
        displayPrimary[0][0] * 20, displayPrimary[0][1] * 20,
        displayPrimary[1][0] * 20, displayPrimary[1][1] * 20,
        displayPrimary[2][0] * 20, displayPrimary[2][1] * 20,
        whitePoint[0] * 20, whitePoint[1] * 20);

    wp_image_description_creator_params_v1_set_mastering_luminance(
        wlWm.hdrImageCreator,
        minDisplayLuminance, maxDisplayLuminance);
  }
  else if (hdrPQ && hdrMetadata && wlWm.cmHasMasteringPrimaries)
    DEBUG_WARN("HDR mastering target volume is invalid or unsupported; "
        "omitting mastering display metadata");

  if (hdrPQ && hdrMetadata)
  {
    if (maxCLL > 0)
      wp_image_description_creator_params_v1_set_max_cll(
          wlWm.hdrImageCreator, maxCLL);
    if (maxFALL > 0 && (maxCLL == 0 || maxFALL <= maxCLL))
      wp_image_description_creator_params_v1_set_max_fall(
          wlWm.hdrImageCreator, maxFALL);
  }

  wlWm.hdrImageDesc =
    wp_image_description_creator_params_v1_create(wlWm.hdrImageCreator);
  wlWm.hdrImageCreator = NULL; // consumed by create

  if (!wlWm.hdrImageDesc)
  {
    DEBUG_WARN("Failed to create HDR image description");
    LG_UNLOCK(wlWm.hdrLock);
    return;
  }

  wlWm.hdrImageDescPQ = hdrPQ;
  wlWm.hdrImageDescReady = false;
  wp_image_description_v1_add_listener(
      wlWm.hdrImageDesc, &hdrImageDescListener, NULL);
  LG_UNLOCK(wlWm.hdrLock);

  DEBUG_INFO("HDR image description requested (%s, %s, "
      "maxLum:%u cd/m² minLum:%u (0.0001 cd/m²) maxCLL:%u maxFALL:%u)",
      hdrPQ ? "PQ" : "scRGB", hdrPQ ? "BT.2020" : "sRGB",
      maxDisplayLuminance, minDisplayLuminance, maxCLL, maxFALL);
}

bool waylandRequestHDR(const uint16_t displayPrimary[3][2],
    const uint16_t whitePoint[2], uint32_t maxDisplayLuminance,
    uint32_t minDisplayLuminance, uint32_t maxCLL, uint32_t maxFALL,
    bool hdrPQ, bool hdrMetadata)
{
  if (!atomic_load_explicit(&wlWm.cmFeaturesDone, memory_order_acquire))
    return false;
  if (!wlWm.cmHasPerceptualIntent)
    return false;
  if (hdrPQ && (!wlWm.cmHasParametric || !wlWm.cmHasTFSt2084PQ ||
        !wlWm.cmHasPrimariesBT2020))
    return false;
  if (!hdrPQ && !wlWm.cmHasWindowsSCRGB)
    return false;

  atomic_store(&wlWm.hdrRequestedPQ, hdrPQ);
  atomic_store(&wlWm.hdrRequested, true);

  LG_LOCK(wlWm.pendingHDRLock);
  wlWm.pendingHDR.pq       = hdrPQ;
  wlWm.pendingHDR.metadata = hdrMetadata;
  memcpy(wlWm.pendingHDR.displayPrimary, displayPrimary,
      sizeof(wlWm.pendingHDR.displayPrimary));
  memcpy(wlWm.pendingHDR.whitePoint, whitePoint,
      sizeof(wlWm.pendingHDR.whitePoint));
  wlWm.pendingHDR.maxDisplayLuminance = maxDisplayLuminance;
  wlWm.pendingHDR.minDisplayLuminance = minDisplayLuminance;
  wlWm.pendingHDR.maxCLL              = maxCLL;
  wlWm.pendingHDR.maxFALL             = maxFALL;
  wlWm.pendingHDRAction = WAYLAND_HDR_PENDING_APPLY;
  LG_UNLOCK(wlWm.pendingHDRLock);
  return true;
}

void waylandRequestClearHDR(void)
{
  atomic_store(&wlWm.hdrRequested, false);
  LG_LOCK(wlWm.pendingHDRLock);
  wlWm.pendingHDRAction = WAYLAND_HDR_PENDING_CLEAR;
  LG_UNLOCK(wlWm.pendingHDRLock);
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
