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
  void (*callback)(void * udata, int * id);
}
ConfigCallback;

typedef struct OverlayConfig
{
  struct ll * callbacks;
  struct ll * tabCallbacks;
}
OverlayConfig;

static OverlayConfig cfg = { 0 };

static bool config_init(void ** udata, const void * params)
{
  cfg.callbacks    = ll_new();
  cfg.tabCallbacks = ll_new();
  if (!cfg.callbacks)
  {
    DEBUG_ERROR("failed to allocate ram");
    return false;
  }

  return true;
}

static void config_freeList(struct ll * list)
{
  ConfigCallback * cb;
  while(ll_shift(list, (void **)&cb))
    free(cb);
  ll_free(list);
}

static void config_free(void * udata)
{
  config_freeList(cfg.callbacks);
  config_freeList(cfg.tabCallbacks);
}

static void config_renderLGTab(void)
{
  const float fontSize = igGetFontSize();

  if (igCollapsingHeaderBoolPtr("About", NULL,
        ImGuiTreeNodeFlags_DefaultOpen))
  {
    igText(LG_COPYRIGHT_STR);
    overlayTextURL(LG_WEBSITE_STR, NULL);
    igText(LG_VERSION_STR);
    igSeparator();
    igTextWrapped(LG_LICENSE_STR);
  }

  if (igCollapsingHeaderBoolPtr("Help & Support", NULL,
        ImGuiTreeNodeFlags_DefaultOpen))
  {
    igBeginTable("split", 2, 0, (ImVec2){}, 0.0f);
    igTableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, fontSize * 9.0f, 0);
    for(const StringPair * help = LG_HELP_LINKS; help->name; ++help)
    {
      igTableNextColumn();
      igBulletText(help->name);
      igTableNextColumn();
      overlayTextMaybeURL(help->value, true);
    }
    igEndTable();
  }

  if (igCollapsingHeaderBoolPtr("The Looking Glass Team / Donations", NULL,
        ImGuiTreeNodeFlags_DefaultOpen))
  {
    for(const struct LGTeamMember * member = LG_TEAM; member->name; ++member)
    {
      if (igTreeNodeStr(member->name))
      {
        igSpacing();
        igTextWrapped(member->blurb);
        if (member->donate[0].name)
        {
          igSeparator();
          igTextWrapped(
              "If you would like to show financial support for his work you can "
              "do so directly via the following platform%s:",
              member->donate[1].name ? "s" : "");

          igBeginTable("split", 2, 0, (ImVec2){}, 0.0f);
          igTableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
              fontSize * 10.0f, 0);
          for(const StringPair * donate = member->donate; donate->name; ++donate)
          {
            igTableNextColumn();
            igBulletText(donate->name);
            igTableNextColumn();
            overlayTextMaybeURL(donate->value, false);
          }
          igEndTable();
        }
        igTreePop();
        igSeparator();
      }
    }
  }
}

static int config_render(void * udata, bool interactive, struct Rect * windowRects,
    int maxRects)
{
  if (!interactive)
    return 0;

  int id = 1000;

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

  igPushIDInt(id++);
  if (!igBegin("Configuration", NULL, 0))
  {
    overlayGetImGuiRect(windowRects);
    igEnd();
    igPopID();
    return 1;
  }

  igBeginTabBar("Configuration#tabs", 0);

  if (igBeginTabItem("Looking Glass", NULL, 0))
  {
    config_renderLGTab();
    igEndTabItem();
  }

  ConfigCallback * cb;

  if (igBeginTabItem("Settings", NULL, 0))
  {
    for (ll_reset(cfg.callbacks); ll_walk(cfg.callbacks, (void **)&cb); )
    {
      if (!igCollapsingHeaderBoolPtr(cb->title, NULL, 0))
        continue;

      igPushIDInt(id++);
      cb->callback(cb->udata, &id);
      igPopID();
    }
    igEndTabItem();
  }

  for (ll_reset(cfg.tabCallbacks); ll_walk(cfg.tabCallbacks, (void **)&cb); )
  {
    if (!igBeginTabItem(cb->title, NULL, 0))
      continue;

    igPushIDInt(id++);
    cb->callback(cb->udata, &id);
    igPopID();
    igEndTabItem();
  }

  igEndTabBar();

  overlayGetImGuiRect(windowRects);
  igEnd();
  igPopID();
  return 1;
}

struct LG_OverlayOps LGOverlayConfig =
{
  .name           = "Config",
  .init           = config_init,
  .free           = config_free,
  .render         = config_render
};

static void config_addToList(struct ll * list, const char * title,
    void(*callback)(void * udata, int * id), void * udata)
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
  ll_push(list, cb);
}

void overlayConfig_register(const char * title,
    void (*callback)(void * udata, int * id), void * udata)
{
  config_addToList(cfg.callbacks, title, callback, udata);
};

void overlayConfig_registerTab(const char * title,
    void (*callback)(void * udata, int * id), void * udata)
{
  config_addToList(cfg.tabCallbacks, title, callback, udata);
};
