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

#include "../overlays.h"
#include "../main.h"
#include "overlay_utils.h"
#include "common/array.h"

#include "version.h"
#include "common/appstrings.h"
#include "common/stringlist.h"
#include "common/stringutils.h"

#include "resources/lg-logo.svg.h"

#include <string.h>
#include <math.h>

#define SEGMENTS 12

static bool         l_show;
static bool         l_fadeDone;
static float        l_alpha;
static OverlayImage l_logo;
static float        l_vectors[SEGMENTS][2];
static StringList   l_tagline;
static StringList   l_footline;

static void calcRadialVectors(float vectors[][2], int segments)
{
  for (unsigned int i = 0; i < segments; ++i)
  {
    float angle = (i / (float)(segments - 1)) * M_PI * 2.0f;
    vectors[i][0] = cos(angle);
    vectors[i][1] = sin(angle);
  }
}

static void drawRadialGradient(ImDrawList * list, int x, int y, int w, int h,
    ImU32 innerColor, ImU32 outerColor,
    float vectors[0][2], int segments)
{
  const ImVec2 uv = list->_Data->TexUvWhitePixel;

  ImDrawList_PrimReserve(list, (segments - 1) * 3, segments + 1);
  for(int i = 0; i < segments - 1; ++i)
  {
    ImDrawList_PrimWriteIdx(list, list->_VtxCurrentIdx);
    ImDrawList_PrimWriteIdx(list, list->_VtxCurrentIdx + i + 1);
    ImDrawList_PrimWriteIdx(list, list->_VtxCurrentIdx + i + 2);
  }

  ImDrawList_PrimWriteVtx(list,
      (ImVec2){x, y},
      uv,
      innerColor);

  for (unsigned int i = 0; i < segments; ++i)
    ImDrawList_PrimWriteVtx(list,
        (ImVec2){
          x + vectors[i][0] * w,
          y + vectors[i][1] * h
        },
        uv,
        outerColor);
}

static bool splash_init(void ** udata, const void * params)
{
  l_show     = true;
  l_fadeDone = false;
  l_alpha    = 1.0f;

  overlayLoadSVG(b_lg_logo_svg, b_lg_logo_svg_size, &l_logo, 200, 200);
  calcRadialVectors(l_vectors, ARRAY_LENGTH(l_vectors));

  l_tagline  = stringlist_new(false);
  l_footline = stringlist_new(false);

  stringlist_push(l_tagline, "Looking Glass");
  stringlist_push(l_tagline, (char *)LG_WEBSITE_URL);

  stringlist_push(l_footline, (char *)LG_VERSION_STR);
  stringlist_push(l_footline, (char *)LG_COPYRIGHT_STR);

  return true;
}

static void splash_free(void * udata)
{
  overlayFreeImage(&l_logo);
  stringlist_free(&l_tagline );
  stringlist_free(&l_footline);
}

static void renderText(ImDrawList * list, int x, int y, ImU32 color,
    StringList lines, bool topAlign)
{
  static float textHeight = 0.0f;
  ImVec2 size;

  if (textHeight == 0.0f)
  {
    const char * tmp = "W";
    igCalcTextSize(&size, tmp, tmp + 1, false, 0.0f);
    textHeight = size.y;
  }

  float fy = y;
  const unsigned int count = stringlist_count(lines);
  for(int i = 0; i < count; ++i)
  {
    const char * text = stringlist_at(lines, topAlign ? i : count - i - 1);
    const int    len  = strlen(text);

    igCalcTextSize(&size, text, text + len, false, 0.0f);
    ImDrawList_AddText_Vec2(
      list,
      (ImVec2){
       x - size.x / 2,
       topAlign ? fy : fy - size.y
      },
      color,
      text,
      text + len
    );

    if (topAlign)
      fy += textHeight;
    else
      fy -= textHeight;
  }
}

static int splash_render(void * udata, bool interactive, struct Rect * windowRects,
    int maxRects)
{
  if (!l_show && l_fadeDone)
    return 0;

  const float  alpha  = l_fadeDone ? 1.0f : l_alpha;
  ImVec2     * screen = overlayGetScreenSize();
  ImDrawList * list   = igGetBackgroundDrawList_Nil();

  struct Rect rect = {
    .x = 0,
    .y = 0,
    .w = screen->x,
    .h = screen->y
  };

  struct Rect logoRect = {
    .x = screen->x / 2 - l_logo.width  / 2,
    .y = screen->y / 2 - l_logo.height / 2,
    .w = l_logo.width,
    .h = l_logo.height
  };

  const ImU32 innerColor = igColorConvertFloat4ToU32((ImVec4){
      0.234375f, 0.015625f, 0.425781f, alpha});
  const ImU32 outerColor = igColorConvertFloat4ToU32((ImVec4){
      0.0f, 0.0f, 0.0f, alpha});
  const ImU32 imageColor = igColorConvertFloat4ToU32((ImVec4){
      1.0f, 1.0f, 1.0f, alpha});
  const ImU32 fontColor  = igColorConvertFloat4ToU32((ImVec4){
      0.8f, 0.8f, 0.8f, alpha});

  drawRadialGradient(list,
      screen->x / 2, screen->y / 2,
      screen->x    , screen->y    ,
      innerColor,
      outerColor,
      l_vectors,
      ARRAY_LENGTH(l_vectors));

  ImDrawList_AddImage(
    list,
    l_logo.tex,
    (ImVec2){
      logoRect.x,
      logoRect.y
    },
    (ImVec2){
      logoRect.x + logoRect.w,
      logoRect.y + logoRect.h
    },
    (ImVec2){ 0, 0 },
    (ImVec2){ 1, 1 },
    imageColor);

  renderText(list,
      screen->x / 2,
      logoRect.y + logoRect.h + 10,
      fontColor,
      l_tagline,
      true);

  renderText(list,
      screen->x / 2,
      screen->y - 10,
      fontColor,
      l_footline,
      false);

  *windowRects = rect;
  return 1;
}

static bool splash_tick(void * udata, unsigned long long tickCount)
{
  if (!l_show && !l_fadeDone)
  {
    if (g_params.quickSplash)
    {
      l_fadeDone = true;
      return true;
    }

    l_alpha -= 1.0f / TICK_RATE;
    if (l_alpha <= 0.0f)
      l_fadeDone = true;
    return true;
  }

  return false;
}

struct LG_OverlayOps LGOverlaySplash =
{
  .name   = "splash",
  .init   = splash_init,
  .free   = splash_free,
  .render = splash_render,
  .tick   = splash_tick,
};

void overlaySplash_show(bool show)
{
  if (l_show == show)
    return;

  l_show = show;
  app_invalidateOverlay(true);
}
