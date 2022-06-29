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

#include "common/option.h"

#include "../main.h"

static bool showFPS;

static void showFPSKeybind(int sc, void * opaque)
{
  showFPS ^= true;
  app_invalidateWindow(false);
}

static void fps_earlyInit(void)
{
  static struct Option options[] =
  {
    {
      .module         = "win",
      .name           = "showFPS",
      .description    = "Enable the FPS & UPS display",
      .shortopt       = 'k',
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = false,
    },
    { 0 }
  };
  option_register(options);
}

static bool fps_init(void ** udata, const void * params)
{
  app_registerKeybind(0, 'D', showFPSKeybind, NULL, "FPS display toggle");
  showFPS = option_get_bool("win", "showFPS");
  return true;
}

static void fps_free(void * udata)
{
}

static int fps_render(void * udata, bool interactive, struct Rect * windowRects,
    int maxRects)
{
  if (!showFPS)
    return 0;

  ImVec2 pos = {0.0f, 0.0f};
  igSetNextWindowBgAlpha(0.6f);
  igSetNextWindowPos(pos, ImGuiCond_FirstUseEver, pos);
  igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2) { 4.0f , 4.0f });
  igPushStyleVar_Vec2(ImGuiStyleVar_WindowMinSize, (ImVec2) { 0.0f , 0.0f });

  igBegin(
    "FPS",
    NULL,
    ImGuiWindowFlags_NoDecoration       | ImGuiWindowFlags_AlwaysAutoResize |
    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav            |
    ImGuiWindowFlags_NoTitleBar
  );

  igText("FPS:%4.2f UPS:%4.2f",
      atomic_load_explicit(&g_state.fps, memory_order_relaxed),
      atomic_load_explicit(&g_state.ups, memory_order_relaxed));

  overlayGetImGuiRect(windowRects);
  igEnd();

  igPopStyleVar(2);

  return 1;
}

struct LG_OverlayOps LGOverlayFPS =
{
  .name           = "FPS",
  .earlyInit      = fps_earlyInit,
  .init           = fps_init,
  .free           = fps_free,
  .render         = fps_render
};
