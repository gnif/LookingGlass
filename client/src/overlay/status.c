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

#include "interface/overlay.h"
#include "math.h"
#include "cimgui.h"

#include "../overlays.h"
#include "../main.h"
#include "overlay_utils.h"

#include "resources/status/recording.svg.h"
#include "resources/status/spice.svg.h"

//TODO: Make this user configurable?
#define ICON_SIZE 32

static bool         l_state[LG_USER_STATUS_MAX] = { 0 };
static OverlayImage l_image[LG_USER_STATUS_MAX] = { 0 };
static bool         l_recordToggle;
static double       l_scale = 1.0;

static void status_loadImage(const char * data, unsigned int size,
    OverlayImage * image, int width, int height)
{
  overlayFreeImage(image);
  overlayLoadSVG(data, size, image, width, height);
}

static void status_loadIcons(double scale)
{
  int iconSize = ceil(scale * ICON_SIZE);

  status_loadImage(b_status_recording_svg, b_status_recording_svg_size,
      &l_image[LG_USER_STATUS_RECORDING], iconSize, iconSize);

  status_loadImage(b_status_spice_svg, b_status_spice_svg_size,
      &l_image[LG_USER_STATUS_SPICE], iconSize, iconSize);
}

static bool status_init(void ** udata, const void * params)
{
  status_loadIcons(l_scale);
  return true;
}

static void status_free(void * udata)
{
  for(int i = 0; i < LG_USER_STATUS_MAX; ++i)
    overlayFreeImage(&l_image[i]);
}

static int status_render(void * udata, bool interactive, struct Rect * windowRects,
    int maxRects)
{
  const int marginX = 10;
  const int marginY = 10;
  const int gapX    = 5;

  if (g_state.windowScale > l_scale)
  {
    l_scale = g_state.windowScale;
    status_loadIcons(l_scale);
  }

  ImVec2 * screen  = overlayGetScreenSize();
  struct Rect rect = {
    .x = screen->x - LG_USER_STATUS_MAX * (ICON_SIZE + gapX) - marginX,
    .y = marginY,
    .w = LG_USER_STATUS_MAX * (ICON_SIZE + gapX),
    .h = ICON_SIZE
  };

  int xPos = screen->x - marginX;
  for(int i = 0; i < LG_USER_STATUS_MAX; ++i)
  {
    OverlayImage * img = &l_image[i];
    if (!l_state[i] || !img->tex)
      continue;

    // if the recording indicator is off, don't draw but reserve space
    if (i == LG_USER_STATUS_RECORDING && !l_recordToggle)
      goto next;

    ImDrawList_AddImage(
      igGetBackgroundDrawList_Nil(),
      img->tex,
      (ImVec2){
        xPos,
        marginY
      },
      (ImVec2){
        xPos - ICON_SIZE,
        img->height / l_scale + marginY
      },
      (ImVec2){ 0, 0 },
      (ImVec2){ 1, 1 },
      0xFFFFFFFF);

next:
    xPos -= ICON_SIZE + gapX;
  }

  *windowRects = rect;
  return 1;
}

static bool status_tick(void * udata, unsigned long long tickCount)
{
  static unsigned long long lastTick = 0;

  if (tickCount - lastTick >= 25)
  {
    l_recordToggle = !l_recordToggle;
    lastTick = tickCount;
    return true;
  }

  return false;
}

struct LG_OverlayOps LGOverlayStatus =
{
  .name           = "status",
  .init           = status_init,
  .free           = status_free,
  .render         = status_render,
  .tick           = status_tick,
};

void overlayStatus_set(LGUserStatus status, bool value)
{
  if (l_state[status] == value)
    return;

  l_state[status] = value;
  app_invalidateOverlay(true);
};
