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
#include "cimgui.h"
#include "overlay_utils.h"

#include "common/stringutils.h"

#include "../main.h"

static bool recordShow = false;
static bool recordToggle = false;
static unsigned long long lastTick = 0;

static bool record_init(void ** udata, const void * params)
{
  return true;
}

static void record_free(void * udata)
{}

static int record_render(void * udata, bool interactive, struct Rect * windowRects,
    int maxRects)
{
  if (!recordShow || !recordToggle)
    return 0;

  ImVec2 * screen = overlayGetScreenSize();
  ImDrawList_AddCircleFilled(igGetBackgroundDrawList_Nil(),
    (ImVec2) { screen->x - 20.0f, 20.0f },
    5.0f, 0xFF0000FF, 0
  );

  *windowRects = (struct Rect) {
    .x = screen->x - 26, .y = 14, .w = 12, .h = 12
  };
  return 1;
}

static bool record_tick(void * udata, unsigned long long tickCount)
{
  if (tickCount - lastTick >= 25)
  {
    recordToggle = !recordToggle;
    lastTick = tickCount;
    return true;
  }
  return false;
}

struct LG_OverlayOps LGOverlayRecord =
{
  .name           = "record",
  .init           = record_init,
  .free           = record_free,
  .render         = record_render,
  .tick           = record_tick,
};

void overlayRecord_show(bool show)
{
  if (show == recordShow)
    return;

  recordShow = show;
  app_invalidateOverlay(true);
}
