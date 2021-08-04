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

#include "../main.h"

static bool alert_init(void ** udata, const void * params)
{
  return true;
}

static void alert_free(void * udata)
{
}

static const uint32_t colours[] =
{
  0xCC0000, // LG_ALERT_INFO
  0x00CC00, // LG_ALERT_SUCCESS
  0x007FCC, // LG_ALERT_WARNING
  0x0000FF  // LG_ALERT_ERROR
};

static int alert_render(void * udata, bool interactive, struct Rect * windowRects,
    int maxRects)
{
  if (!g_state.alertShow)
    return 0;

  ImVec2 * screen = overlayGetScreenSize();
  igSetNextWindowBgAlpha(0.8f);
  igSetNextWindowPos((ImVec2) { screen->x / 2.0f, screen->y / 2.0f }, 0,
    (ImVec2) { 0.5f, 0.5f });
  igPushStyleColorU32(ImGuiCol_WindowBg, colours[g_state.alertType]);
  igPushStyleVarVec2(ImGuiStyleVar_WindowPadding, (ImVec2) { 4.0f , 4.0f });
  igPushStyleVarVec2(ImGuiStyleVar_WindowMinSize, (ImVec2) { 0.0f , 0.0f });

  igBegin(
    "Alert",
    NULL,
    ImGuiWindowFlags_NoDecoration    | ImGuiWindowFlags_AlwaysAutoResize   |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
    ImGuiWindowFlags_NoNav           | ImGuiWindowFlags_NoTitleBar
  );

  igPushFont(g_state.fontLarge);
  igText("%s", g_state.alertMessage);
  igPopFont();

  overlayGetImGuiRect(windowRects);
  igEnd();

  igPopStyleVar(2);
  igPopStyleColor(1);

  return 1;
}

struct LG_OverlayOps LGOverlayAlert =
{
  .name           = "alert",
  .init           = alert_init,
  .free           = alert_free,
  .render         = alert_render
};
