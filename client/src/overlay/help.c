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

#include "interface/overlay.h"
#include "cimgui.h"
#include "overlay_utils.h"

#include "common/debug.h"

#include "../kb.h"
#include "../main.h"

static bool help_init(void ** udata, const void * params)
{
  return true;
}

static void help_free(void * udata)
{
}

static int help_render(void * udata, bool interactive, struct Rect * windowRects,
    int maxRects)
{
  if (!g_state.escapeHelp)
    return 0;

  ImVec2 * screen = overlayGetScreenSize();
  igSetNextWindowBgAlpha(0.6f);
  igSetNextWindowPos((ImVec2) { 0.0f, screen->y }, 0, (ImVec2) { 0.0f, 1.0f });

  igBegin(
    "Help",
    NULL,
    ImGuiWindowFlags_NoDecoration    | ImGuiWindowFlags_AlwaysAutoResize   |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
    ImGuiWindowFlags_NoNav           | ImGuiWindowFlags_NoTitleBar
  );

  if (igBeginTable("Help", 2, 0, (ImVec2) { 0.0f, 0.0f }, 0.0f))
  {
    const char * escapeName = linux_to_display[g_params.escapeKey];

    igTableNextColumn();
    igText("%s", escapeName);
    igTableNextColumn();
    igText("Toggle capture mode");

    for (int i = 0; i < KEY_MAX; ++i)
      if (g_state.keyDescription[i])
      {
        igTableNextColumn();
        igText("%s+%s", escapeName, linux_to_display[i]);
        igTableNextColumn();
        igText(g_state.keyDescription[i]);
      }

    igEndTable();
  }

  overlayGetImGuiRect(windowRects);
  igEnd();

  return 1;
}

struct LG_OverlayOps LGOverlayHelp =
{
  .name           = "Help",
  .init           = help_init,
  .free           = help_free,
  .render         = help_render
};
