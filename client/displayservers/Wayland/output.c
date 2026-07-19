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
#include <stdlib.h>
#include <string.h>

#include <wayland-client.h>
#include <unistd.h>

#include "app.h"
#include "common/debug.h"

struct OutputColorDescription
{
  struct WaylandOutput * output;
  struct wp_image_description_v1 * description;
  struct wp_image_description_info_v1 * info;
};

static void outputColorDescriptionFree(struct OutputColorDescription * desc)
{
  if (!desc)
    return;

  if (desc->info)
    wl_proxy_destroy((struct wl_proxy *)desc->info);
  if (desc->description)
    wp_image_description_v1_destroy(desc->description);
  if (desc->output && desc->output->colorDescription == desc)
    desc->output->colorDescription = NULL;
  free(desc);
}

static struct WaylandOutput * outputFind(struct wl_output * output)
{
  struct WaylandOutput * node;
  wl_list_for_each(node, &wlWm.outputs, link)
    if (node->output == output)
      return node;
  return NULL;
}

void waylandOutputUpdateHDRWhiteLevel(void)
{
  uint32_t pqWhite = 203;
  uint32_t scRGBWhite = 80;
  struct SurfaceOutput * surfaceOutput;

  wl_list_for_each(surfaceOutput, &wlWm.surfaceOutputs, link)
  {
    struct WaylandOutput * output = outputFind(surfaceOutput->output);
    if (!output || !output->referenceWhiteValid)
      continue;

    pqWhite = scRGBWhite = output->referenceWhiteLevel;
    break;
  }

  const uint32_t oldPQ = atomic_exchange(&wlWm.hdrPQWhiteLevel, pqWhite);
  const uint32_t oldScRGB =
    atomic_exchange(&wlWm.hdrScRGBWhiteLevel, scRGBWhite);
  if ((oldPQ != pqWhite || oldScRGB != scRGBWhite) && wlWm.frameEvent)
  {
    DEBUG_INFO("Wayland output reference white: %u cd/m²", pqWhite);
    app_invalidateWindow(true);
    waylandStopWaitFrame();
  }
}

static void outputInfoDone(void * data,
    struct wp_image_description_info_v1 * info)
{
  struct OutputColorDescription * desc = data;
  desc->info = NULL; // the protocol destroys it after this event
  outputColorDescriptionFree(desc);
}

static void outputInfoICCFile(void * data,
    struct wp_image_description_info_v1 * info, int32_t fd, uint32_t size)
{
  close(fd);
}

static void outputInfoPrimaries(void * data,
    struct wp_image_description_info_v1 * info,
    int32_t rx, int32_t ry, int32_t gx, int32_t gy,
    int32_t bx, int32_t by, int32_t wx, int32_t wy)
{
}

static void outputInfoPrimariesNamed(void * data,
    struct wp_image_description_info_v1 * info, uint32_t primaries)
{
}

static void outputInfoTFPower(void * data,
    struct wp_image_description_info_v1 * info, uint32_t exponent)
{
}

static void outputInfoTFNamed(void * data,
    struct wp_image_description_info_v1 * info, uint32_t tf)
{
}

static void outputInfoLuminances(void * data,
    struct wp_image_description_info_v1 * info,
    uint32_t minLum, uint32_t maxLum, uint32_t referenceLum)
{
  struct OutputColorDescription * desc = data;
  if (!referenceLum || desc->output->colorDescription != desc)
    return;

  desc->output->referenceWhiteLevel = referenceLum;
  desc->output->referenceWhiteValid = true;
  waylandOutputUpdateHDRWhiteLevel();
}

static void outputInfoTargetPrimaries(void * data,
    struct wp_image_description_info_v1 * info,
    int32_t rx, int32_t ry, int32_t gx, int32_t gy,
    int32_t bx, int32_t by, int32_t wx, int32_t wy)
{
}

static void outputInfoTargetLuminance(void * data,
    struct wp_image_description_info_v1 * info,
    uint32_t minLum, uint32_t maxLum)
{
}

static void outputInfoTargetMaxCLL(void * data,
    struct wp_image_description_info_v1 * info, uint32_t maxCLL)
{
}

static void outputInfoTargetMaxFALL(void * data,
    struct wp_image_description_info_v1 * info, uint32_t maxFALL)
{
}

static const struct wp_image_description_info_v1_listener outputInfoListener =
{
  .done              = outputInfoDone,
  .icc_file          = outputInfoICCFile,
  .primaries         = outputInfoPrimaries,
  .primaries_named   = outputInfoPrimariesNamed,
  .tf_power          = outputInfoTFPower,
  .tf_named          = outputInfoTFNamed,
  .luminances        = outputInfoLuminances,
  .target_primaries  = outputInfoTargetPrimaries,
  .target_luminance  = outputInfoTargetLuminance,
  .target_max_cll    = outputInfoTargetMaxCLL,
  .target_max_fall   = outputInfoTargetMaxFALL,
};

static void outputDescriptionFailed(void * data,
    struct wp_image_description_v1 * description,
    uint32_t cause, const char * message)
{
  struct OutputColorDescription * desc = data;
  DEBUG_WARN("Failed to query Wayland output colour description "
      "(cause:%u): %s", cause, message);
  outputColorDescriptionFree(desc);
}

static void outputDescriptionReady(struct OutputColorDescription * desc)
{
  desc->info = wp_image_description_v1_get_information(desc->description);
  if (!desc->info)
  {
    outputColorDescriptionFree(desc);
    return;
  }
  wp_image_description_info_v1_add_listener(
      desc->info, &outputInfoListener, desc);
}

static void outputDescriptionReadyV1(void * data,
    struct wp_image_description_v1 * description, uint32_t identity)
{
  outputDescriptionReady(data);
}

static void outputDescriptionReadyV2(void * data,
    struct wp_image_description_v1 * description,
    uint32_t identityHi, uint32_t identityLo)
{
  outputDescriptionReady(data);
}

static const struct wp_image_description_v1_listener outputDescriptionListener =
{
  .failed = outputDescriptionFailed,
  .ready  = outputDescriptionReadyV1,
  .ready2 = outputDescriptionReadyV2,
};

static void outputRequestColorDescription(struct WaylandOutput * output)
{
  outputColorDescriptionFree(output->colorDescription);
  output->referenceWhiteValid = false;
  waylandOutputUpdateHDRWhiteLevel();

  struct OutputColorDescription * desc = calloc(1, sizeof(*desc));
  if (!desc)
    return;

  desc->output = output;
  desc->description =
    wp_color_management_output_v1_get_image_description(output->colorOutput);
  if (!desc->description)
  {
    free(desc);
    return;
  }

  output->colorDescription = desc;
  wp_image_description_v1_add_listener(
      desc->description, &outputDescriptionListener, desc);
}

static void outputImageDescriptionChanged(void * data,
    struct wp_color_management_output_v1 * colorOutput)
{
  outputRequestColorDescription(data);
}

static const struct wp_color_management_output_v1_listener colorOutputListener =
{
  .image_description_changed = outputImageDescriptionChanged,
};

void waylandOutputColorMgmtInit(struct WaylandOutput * output)
{
  if (!wlWm.colorManager || output->colorOutput)
    return;

  output->colorOutput =
    wp_color_manager_v1_get_output(wlWm.colorManager, output->output);
  if (!output->colorOutput)
    return;

  wp_color_management_output_v1_add_listener(
      output->colorOutput, &colorOutputListener, output);
  outputRequestColorDescription(output);
}

void waylandOutputColorMgmtInitAll(void)
{
  struct WaylandOutput * output;
  wl_list_for_each(output, &wlWm.outputs, link)
    waylandOutputColorMgmtInit(output);
}

static void outputUpdateScale(struct WaylandOutput * node)
{
  struct WaylandScale original = node->scale;

  if (!wlWm.useFractionalScale || !wlWm.viewporter || !node->logicalWidth)
    node->scale = waylandScaleFromInt(node->scaleInt);
  else
  {
    int32_t modeWidth = node->modeRotate ? node->modeHeight : node->modeWidth;
    node->scale = waylandScaleFromRatio(modeWidth, node->logicalWidth);
  }

  if (!waylandScaleEqual(original, node->scale))
    waylandWindowUpdateScale();
}

static void outputGeometryHandler(void * opaque, struct wl_output * output, int32_t x, int32_t y,
    int32_t physical_width, int32_t physical_height, int32_t subpixel, const char * make,
    const char * model, int32_t output_transform)
{
  struct WaylandOutput * node = opaque;

  switch (output_transform)
  {
    case WL_OUTPUT_TRANSFORM_90:
    case WL_OUTPUT_TRANSFORM_270:
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
      node->modeRotate = true;
      break;

    default:
      node->modeRotate = false;
  }
}

static void outputModeHandler(void * opaque, struct wl_output * wl_output, uint32_t flags,
    int32_t width, int32_t height, int32_t refresh)
{
  if (!(flags & WL_OUTPUT_MODE_CURRENT))
    return;

  struct WaylandOutput * node = opaque;
  node->modeWidth  = width;
  node->modeHeight = height;
}

static void outputDoneHandler(void * opaque, struct wl_output * output)
{
  struct WaylandOutput * node = opaque;
  outputUpdateScale(node);
}

static void outputScaleHandler(void * opaque, struct wl_output * output, int32_t scale)
{
  struct WaylandOutput * node = opaque;
  node->scaleInt = scale;
}

static const struct wl_output_listener outputListener = {
  .geometry = outputGeometryHandler,
  .mode = outputModeHandler,
  .done = outputDoneHandler,
  .scale = outputScaleHandler,
};

static void xdgOutputLogicalPositionHandler(void * opaque, struct zxdg_output_v1 * xdgOutput,
    int32_t x, int32_t y)
{
  // Do nothing.
}

static void xdgOutputLogicalSizeHandler(void * opaque, struct zxdg_output_v1 * xdgOutput,
    int32_t width, int32_t height)
{
  struct WaylandOutput * node = opaque;
  node->logicalWidth  = width;
  node->logicalHeight = height;
}

static void xdgOutputDoneHandler(void * opaque, struct zxdg_output_v1 * xdgOutput)
{
  struct WaylandOutput * node = opaque;
  outputUpdateScale(node);
}

static void xdgOutputNameHandler(void * opaque, struct zxdg_output_v1 * xdgOutput, const char * name)
{
  // Do nothing.
}

static void xdgOutputDescriptionHandler(void * opaque, struct zxdg_output_v1 * xdgOutput,
    const char * description)
{
  // Do nothing.
}

static const struct zxdg_output_v1_listener xdgOutputListener = {
  .logical_position = xdgOutputLogicalPositionHandler,
  .logical_size = xdgOutputLogicalSizeHandler,
  .done = xdgOutputDoneHandler,
  .name = xdgOutputNameHandler,
  .description = xdgOutputDescriptionHandler,
};

bool waylandOutputInit(void)
{
  wl_list_init(&wlWm.outputs);
  return true;
}

void waylandOutputFree(void)
{
  struct WaylandOutput * node;
  struct WaylandOutput * temp;
  wl_list_for_each_safe(node, temp, &wlWm.outputs, link)
  {
    outputColorDescriptionFree(node->colorDescription);
    if (node->colorOutput)
      wp_color_management_output_v1_destroy(node->colorOutput);
    if (node->version >= 3)
      wl_output_release(node->output);
    if (node->xdgOutput)
      zxdg_output_v1_destroy(node->xdgOutput);
    wl_list_remove(&node->link);
    free(node);
  }
}

void waylandOutputBind(uint32_t name, uint32_t version)
{
  struct WaylandOutput * node = calloc(1, sizeof(struct WaylandOutput));
  if (!node)
    return;

  if (version < 2)
  {
    DEBUG_WARN("wl_output version too old: expected >= 2, got %d", version);
    free(node);
    return;
  }

  node->name    = name;
  node->scale   = waylandScaleFromInt(0);
  node->version = version;
  node->output  = wl_registry_bind(wlWm.registry, name,
      &wl_output_interface, version >= 3 ? 3 : 2);

  if (!node->output)
  {
    DEBUG_ERROR("Failed to bind to wl_output %u\n", name);
    free(node);
    return;
  }

  if (wlWm.xdgOutputManager)
  {
    node->xdgOutput = zxdg_output_manager_v1_get_xdg_output(wlWm.xdgOutputManager, node->output);
    if (node->xdgOutput)
      zxdg_output_v1_add_listener(node->xdgOutput, &xdgOutputListener, node);
  }

  wl_output_add_listener(node->output, &outputListener, node);
  wl_list_insert(&wlWm.outputs, &node->link);
  waylandOutputColorMgmtInit(node);
}

void waylandOutputTryUnbind(uint32_t name)
{
  struct WaylandOutput * node;

  wl_list_for_each(node, &wlWm.outputs, link)
  {
    if (node->name == name)
    {
      struct SurfaceOutput * surfaceOutput;
      struct SurfaceOutput * temp;
      wl_list_for_each_safe(
          surfaceOutput, temp, &wlWm.surfaceOutputs, link)
        if (surfaceOutput->output == node->output)
        {
          wl_list_remove(&surfaceOutput->link);
          free(surfaceOutput);
        }

      outputColorDescriptionFree(node->colorDescription);
      if (node->colorOutput)
        wp_color_management_output_v1_destroy(node->colorOutput);
      if (node->version >= 3)
        wl_output_release(node->output);
      if (node->xdgOutput)
        zxdg_output_v1_destroy(node->xdgOutput);
      wl_list_remove(&node->link);
      free(node);
      waylandWindowUpdateScale();
      waylandOutputUpdateHDRWhiteLevel();
      break;
    }
  }
}

struct WaylandScale waylandOutputGetScale(struct wl_output * output)
{
  struct WaylandOutput * node;

  wl_list_for_each(node, &wlWm.outputs, link)
    if (node->output == output)
      return node->scale;
  return waylandScaleFromInt(0);
}
