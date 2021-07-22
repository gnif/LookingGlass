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

#include "interface/overlay.h"
#include "cimgui.h"

#include "../main.h"

static bool fps_init(void ** udata, void * params)
{
  return true;
}

static void fps_free(void * udata)
{
}

static int fps_render(void * udata, bool interactive, struct Rect * windowRects,
    int maxRects)
{
  if (!g_state.showFPS)
    return 0;

  ImVec2 pos = {0.0f, 0.0f};
  igSetNextWindowBgAlpha(0.6f);
  igSetNextWindowPos(pos, 0, pos);

  igBegin(
    "FPS",
    NULL,
    ImGuiWindowFlags_NoDecoration    | ImGuiWindowFlags_AlwaysAutoResize   |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
    ImGuiWindowFlags_NoNav           | ImGuiWindowFlags_NoTitleBar
  );

  igText("FPS:%4.2f UPS:%4.2f",
      atomic_load_explicit(&g_state.fps, memory_order_relaxed),
      atomic_load_explicit(&g_state.ups, memory_order_relaxed));

  if (maxRects == 0)
  {
    igEnd();
    return -1;
  }

  ImVec2 size;
  igGetWindowPos(&pos);
  igGetWindowSize(&size);
  windowRects[0].x = pos.x;
  windowRects[0].y = pos.y;
  windowRects[0].w = size.x;
  windowRects[0].h = size.y;
  igEnd();

  return 1;
}

struct LG_OverlayOps LGOverlayFPS =
{
  .name           = "FPS",
  .init           = fps_init,
  .free           = fps_free,
  .render         = fps_render
};
