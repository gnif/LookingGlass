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
#include "ll.h"

#include "../main.h"
#include "../overlays.h"
#include "version.h"

#include "common/debug.h"
#include "common/appstrings.h"

typedef struct ConfigCallback
{
  const char * title;
  void * udata;
  void (*callback)(void * udata);
}
ConfigCallback;

typedef struct OverlayConfig
{
  struct ll * callbacks;
}
OverlayConfig;

static OverlayConfig cfg = { 0 };

static bool config_init(void ** udata, const void * params)
{
  cfg.callbacks = ll_new();
  if (!cfg.callbacks)
  {
    DEBUG_ERROR("failed to allocate ram");
    return false;
  }

  return true;
}

static void config_free(void * udata)
{
  ConfigCallback * cb;
  while(ll_shift(cfg.callbacks, (void **)&cb))
    free(cb);

  ll_free(cfg.callbacks);
}

static int config_render(void * udata, bool interactive, struct Rect * windowRects,
    int maxRects)
{
  if (!interactive)
    return 0;

  const float fontSize = igGetFontSize();
  const ImGuiViewport * viewport = igGetMainViewport();
  igSetNextWindowPos(
      (ImVec2){
        viewport->WorkPos.x + 100,
        viewport->WorkPos.y + 30
      },
      ImGuiCond_FirstUseEver,
      (ImVec2){}
  );

  igSetNextWindowSize(
      (ImVec2){550, 680},
      ImGuiCond_FirstUseEver);

  if (!igBegin("Configuration", NULL, ImGuiWindowFlags_MenuBar))
  {
    overlayGetImGuiRect(windowRects);
    igEnd();
    return 1;
  }

  igBeginMenuBar();
  igEndMenuBar();

  if (igCollapsingHeaderBoolPtr("About Looking Glass", NULL, 0))
  {
    igText(LG_COPYRIGHT_STR);
    igText(LG_WEBSITE_STR);
    igText(LG_VERSION_STR);
    igSeparator();
    igTextWrapped(LG_LICENSE_STR);
  }

  if (igCollapsingHeaderBoolPtr("Help & Support", NULL, 0))
  {
    igBeginTable("split", 2, 0, (ImVec2){}, 0.0f);
    igTableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, fontSize * 9.0f, 0);
    for(const StringPair * help = LG_HELP_LINKS; help->name; ++help)
    {
      igTableNextColumn();
      igBulletText(help->name);
      igTableNextColumn();
      igTextWrapped(help->value);
    }
    igEndTable();
  }

  if (igCollapsingHeaderBoolPtr("The Looking Glass Team / Donations", NULL, 0))
  {
    for(const struct LGTeamMember * member = LG_TEAM; member->name; ++member)
    {
      if (igTreeNodeStr(member->name))
      {
        igSpacing();
        igTextWrapped(member->blurb);
        if (member->donate[0].name)
        {
          igSpacing();
          igTextWrapped(
              "If you would like to show financial support for his work you can "
              "do so directly via the following platform(s):");

          igSeparator();
          igBeginTable("split", 2, 0, (ImVec2){}, 0.0f);
          igTableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
              fontSize * 10.0f, 0);
          for(const StringPair * donate = member->donate; donate->name; ++donate)
          {
            igTableNextColumn();
            igBulletText(donate->name);
            igTableNextColumn();
            igText(donate->value);
          }
          igEndTable();
        }
        igTreePop();
        igSeparator();
      }
    }
  }

  ConfigCallback * cb;
  for (ll_reset(cfg.callbacks); ll_walk(cfg.callbacks, (void **)&cb); )
  {
    if (!igCollapsingHeaderBoolPtr(cb->title, NULL, 0))
      continue;
    cb->callback(cb->udata);
  }

  overlayGetImGuiRect(windowRects);
  igEnd();
  return 1;
}

struct LG_OverlayOps LGOverlayConfig =
{
  .name           = "Config",
  .init           = config_init,
  .free           = config_free,
  .render         = config_render
};

void overlayConfig_register(const char * title, void (*callback)(void * udata),
    void * udata)
{
  ConfigCallback * cb = calloc(1, sizeof(*cb));
  if (!cb)
  {
    DEBUG_ERROR("failed to allocate ram");
    return;
  }

  cb->title    = title;
  cb->udata    = udata;
  cb->callback = callback;
  ll_push(cfg.callbacks, cb);
};
