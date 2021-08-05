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

#include "overlay_utils.h"

#include <string.h>

#include "common/open.h"
#include "cimgui.h"
#include "main.h"

void overlayGetImGuiRect(struct Rect * rect)
{
  ImVec2 size;
  ImVec2 pos;

  igGetWindowPos(&pos);
  igGetWindowSize(&size);

  rect->x = pos.x;
  rect->y = pos.y;
  rect->w = size.x;
  rect->h = size.y;
}

ImVec2 * overlayGetScreenSize(void)
{
  return &g_state.io->DisplaySize;
}

static void overlayAddUnderline(ImU32 color)
{
  ImVec2 min, max;
  igGetItemRectMin(&min);
  igGetItemRectMax(&max);
  min.y = max.y;
  ImDrawList_AddLine(igGetWindowDrawList(), min, max, color, 1.0f);
}

void overlayTextURL(const char * url, const char * text)
{
  igText(text ? text : url);

  if (igIsItemHovered(ImGuiHoveredFlags_None))
  {
    if (igIsItemClicked(ImGuiMouseButton_Left))
      lgOpenURL(url);
    overlayAddUnderline(igGetColorU32Vec4(*igGetStyleColorVec4(ImGuiCol_ButtonHovered)));
    igSetMouseCursor(ImGuiMouseCursor_Hand);
    igSetTooltip("Open in browser: %s", url);
  }
}

void overlayTextMaybeURL(const char * text, bool wrapped)
{
  if (strncmp(text, "https://", 8) == 0)
    overlayTextURL(text, NULL);
  else if (wrapped)
    igTextWrapped(text);
  else
    igText(text);
}
