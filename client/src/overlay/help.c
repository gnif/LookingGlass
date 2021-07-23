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
#include "overlay_utils.h"

#include "common/debug.h"

#include "../kb.h"
#include "../main.h"

struct HelpData
{
  char * helpText;
};

static bool help_init(void ** udata, void * params)
{
  *udata = calloc(1, sizeof(struct HelpData));
  if (!udata)
  {
    DEBUG_ERROR("Out of memory");
    return false;
  }
  return true;
}

static void help_free(void * udata)
{
  free(udata);
}

static char * buildHelpText(void)
{
  size_t size   = 50;
  size_t offset = 0;
  char * buffer = malloc(size);

  if (!buffer)
    return NULL;

  const char * escapeName = xfree86_to_display[g_params.escapeKey];

  offset += snprintf(buffer, size, "%s %-10s Toggle capture mode\n", escapeName, "");
  if (offset >= size)
  {
    DEBUG_ERROR("Help string somehow overflowed. This should be impossible.");
    return NULL;
  }

  for (int i = 0; i < KEY_MAX; ++i)
  {
    if (g_state.keyDescription[i])
    {
      const char * keyName = xfree86_to_display[i];
      const char * desc    = g_state.keyDescription[i];
      int needed = snprintf(buffer + offset, size - offset, "%s+%-10s %s\n", escapeName, keyName, desc);
      if (offset + needed < size)
        offset += needed;
      else
      {
        size = size * 2 + needed;
        void * new = realloc(buffer, size);
        if (!new) {
          free(buffer);
          DEBUG_ERROR("Out of memory when constructing help text");
          return NULL;
        }
        buffer = new;
        offset += snprintf(buffer + offset, size - offset, "%s+%-10s %s\n", escapeName, keyName, desc);
      }
    }
  }

  return buffer;
}

static int help_render(void * udata, bool interactive, struct Rect * windowRects,
    int maxRects)
{
  struct HelpData * data = udata;

  if (!g_state.escapeHelp)
  {
    if (data->helpText)
    {
      free(data->helpText);
      data->helpText = NULL;
    }
    return 0;
  }

  if (!data->helpText)
    data->helpText = buildHelpText();

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

  igText("%s", data->helpText);

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
