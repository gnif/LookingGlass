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

#include "resources/lg-logo.svg.h"

#include <math.h>
#include <GL/gl.h>

static bool         l_show;
static bool         l_fadeDone;
static float        l_alpha;
static OverlayImage l_logo;

static bool splash_init(void ** udata, const void * params)
{
  l_show     = true;
  l_fadeDone = false;
  l_alpha    = 1.0f;

  overlayLoadSVG(b_lg_logo_svg, b_lg_logo_svg_size, &l_logo, 200, 200);

  return true;
}

static void splash_free(void * udata)
{
  overlayFreeImage(&l_logo);
}

static void drawRadialGradient(ImDrawList * list, int x, int y, int w, int h,
    int steps, ImU32 innerColor, ImU32 outerColor)
{
  const ImVec2 uv = list->_Data->TexUvWhitePixel;

  ImDrawList_PrimReserve(list, steps * 3, steps + 2);
  for(int i = 0; i < steps; ++i)
  {
    ImDrawList_PrimWriteIdx(list, list->_VtxCurrentIdx);
    ImDrawList_PrimWriteIdx(list, list->_VtxCurrentIdx + i + 1);
    ImDrawList_PrimWriteIdx(list, list->_VtxCurrentIdx + i + 2);
  }

  ImDrawList_PrimWriteVtx(list,
      (ImVec2){x, y},
      uv,
      innerColor);

  for (unsigned int i = 0; i < steps + 1; ++i)
  {
    float angle = (i / (float)steps) * M_PI * 2.0f;
    ImDrawList_PrimWriteVtx(list,
        (ImVec2){
          x + cos(angle) * w,
          y + sin(angle) * h
        },
        uv,
        outerColor);
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

  drawRadialGradient(list,
      screen->x / 2, screen->y / 2,
      screen->x    , screen->y    ,
      12,
      innerColor,
      outerColor);

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

  *windowRects = rect;
  return 1;
}

static bool splash_tick(void * udata, unsigned long long tickCount)
{
  if (!l_show && l_alpha > 0.0f)
  {
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
