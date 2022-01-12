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

#include "msg.h"
#include "interface/overlay.h"
#include "cimgui.h"
#include "overlay_utils.h"

#include "common/stringutils.h"
#include "common/stringlist.h"

#include "../main.h"

#include <string.h>

struct Msg
{
  char * caption;
  char * message;
  StringList lines;
};

struct MsgState
{
  struct ll * messages;
};

struct MsgState l_msg = { 0 };

static bool msg_init(void ** udata, const void * params)
{
  l_msg.messages = ll_new();
  return true;
}

static void freeMsg(struct Msg * msg)
{
  free(msg->caption);
  free(msg->message);
  stringlist_free(&msg->lines);
  free(msg);
}

static void msg_free(void * udata)
{
  struct Msg * msg;
  while(ll_shift(l_msg.messages, (void **)&msg))
    freeMsg(msg);
  ll_free(l_msg.messages);
}

static bool msg_needsOverlay(void * udata)
{
  return ll_count(l_msg.messages) > 0;
}

static int msg_render(void * udata, bool interactive, struct Rect * windowRects,
    int maxRects)
{
  struct Msg * msg;
  if (!ll_peek_head(l_msg.messages, (void **)&msg))
    return 0;

  ImVec2 * screen = overlayGetScreenSize();
  igSetNextWindowBgAlpha(0.8f);
  igSetNextWindowPos((ImVec2) { screen->x * 0.5f, screen->y * 0.5f }, 0,
    (ImVec2) { 0.5f, 0.5f });

  igBegin(
    msg->caption,
    NULL,
    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBringToFrontOnFocus |
    ImGuiWindowFlags_NoSavedSettings  | ImGuiWindowFlags_NoNav |
    ImGuiWindowFlags_NoMove           | ImGuiWindowFlags_NoCollapse
  );

  ImVec2 textSize;

  const int lines = stringlist_count(msg->lines);
  for(int i = 0; i < lines; ++i)
  {
    const char * line = stringlist_at(msg->lines, i);
    if (line[0] == '\0')
    {
      igNewLine();
      continue;
    }

    if (line[0] == '-' && line[1] == '\0')
    {
      igSeparator();
      continue;
    }

    igCalcTextSize(&textSize, line, NULL, false, 0.0);
    igSetCursorPosX((igGetWindowWidth() * 0.5f) - (textSize.x * 0.5f));
    igText("%s", stringlist_at(msg->lines, i));
  }

  igNewLine();
  igCalcTextSize(&textSize, "OK", NULL, false, 0.0);
  ImGuiStyle * style = igGetStyle();
  textSize.x += (style->FramePadding.x * 2.0f) * 8.0f;
  textSize.y += (style->FramePadding.y * 2.0f) * 1.5f;
  igSetCursorPosX((igGetWindowWidth() * 0.5f) - (textSize.x * 0.5f));

  if (igButton("OK", textSize))
  {
    ll_shift(l_msg.messages, NULL);
    freeMsg(msg);
    app_invalidateOverlay(false);
  }

  overlayGetImGuiRect(windowRects);
  igEnd();

  return 1;
}

struct LG_OverlayOps LGOverlayMsg =
{
  .name           = "msg",
  .init           = msg_init,
  .free           = msg_free,
  .needs_overlay  = msg_needsOverlay,
  .render         = msg_render
};

bool overlayMsg_modal(void)
{
  return ll_count(l_msg.messages) > 0;
}

MsgBoxHandle overlayMsg_show(
    const char * caption, const char * fmt, va_list args)
{
  struct Msg * msg = malloc(sizeof(*msg));
  msg->caption = strdup(caption);
  msg->lines   = stringlist_new(false);
  valloc_sprintf(&msg->message, fmt, args);

  char * token = msg->message;
  char * rest  = msg->message;
  do
  {
    if (*rest == '\n')
    {
      *rest = '\0';
      stringlist_push(msg->lines, token);
      token = rest + 1;
    }
    ++rest;
  }
  while(*rest != '\0');

  if (*token)
    stringlist_push(msg->lines, token);

  ll_push(l_msg.messages, msg);
  app_invalidateOverlay(false);

  return (MsgBoxHandle)msg;
}

void overlayMsg_close(MsgBoxHandle * handle)
{
  if (ll_removeData(l_msg.messages, handle))
    freeMsg((struct Msg *)handle);
}
