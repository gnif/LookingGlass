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

#include "wayland.h"

#include <stdbool.h>
#include <string.h>

#include <wayland-client.h>

#include "common/debug.h"

static void outputUpdateScale(struct WaylandOutput * node)
{
  wl_fixed_t original = node->scale;

  if (!wlWm.useFractionalScale || !wlWm.viewporter || !node->logicalWidth)
    node->scale = wl_fixed_from_int(node->scaleInt);
  else
  {
    int32_t modeWidth = node->modeRotate ? node->modeHeight : node->modeWidth;
    node->scale = wl_fixed_from_double(1.0 * modeWidth / node->logicalWidth);
  }

  if (original != node->scale)
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
  node->scale   = 0;
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
}

void waylandOutputTryUnbind(uint32_t name)
{
  struct WaylandOutput * node;

  wl_list_for_each(node, &wlWm.outputs, link)
  {
    if (node->name == name)
    {
      if (node->version >= 3)
        wl_output_release(node->output);
      if (node->xdgOutput)
        zxdg_output_v1_destroy(node->xdgOutput);
      wl_list_remove(&node->link);
      free(node);
      break;
    }
  }
}

wl_fixed_t waylandOutputGetScale(struct wl_output * output)
{
  struct WaylandOutput * node;

  wl_list_for_each(node, &wlWm.outputs, link)
    if (node->output == output)
      return node->scale;
  return 0;
}
