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

#include "overlay_utils.h"

#include <string.h>

#include "common/open.h"
#include "cimgui.h"
#include "main.h"

#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

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
    overlayAddUnderline(igGetColorU32_Vec4(
          *igGetStyleColorVec4(ImGuiCol_ButtonHovered)));
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

bool overlayLoadSVG(const char * data, unsigned int size, OverlayImage * image,
    int width, int height)
{
  image->tex = NULL;

  //nsvgParse alters the data, we need to make a copy and null terminate it
  char * svg = malloc(size + 1);
  if (!svg)
  {
    DEBUG_ERROR("out of ram");
    goto err;
  }

  memcpy(svg, data, size);
  svg[size] = 0;

  NSVGimage * nvi = nsvgParse(svg, "px", 96.0);
  if (!nvi)
  {
    free(svg);
    DEBUG_ERROR("nvsgParseFromData failed");
    goto err;
  }
  free(svg);

  NSVGrasterizer * rast = nsvgCreateRasterizer();
  if (!rast)
  {
    DEBUG_ERROR("nsvgCreateRasterizer failed");
    goto err_image;
  }

  double srcAspect = nvi->width / nvi->height;
  double dstAspect = (double)width / (double)height;
  float  scale;
  if (dstAspect > srcAspect)
  {
    image->width  = (double)height * srcAspect;
    image->height = height;
    scale         = (float)image->width / nvi->width;
  }
  else
  {
    image->width  = width;
    image->height = (double)width / srcAspect;
    scale         = (float)image->height / nvi->height;
  }

  uint8_t * img = malloc(image->width * image->height * 4);
  if (!img)
  {
    DEBUG_ERROR("out of ram");
    goto err_rast;
  }

  nsvgRasterize(rast, nvi,
      0.0f, 0.0f,
      scale,
      img,
      image->width,
      image->height,
      image->width * 4);

  image->tex = RENDERER(createTexture, image->width, image->height, img);
  free(img);

  if (!image->tex)
  {
    DEBUG_ERROR("renderer failed to create the texture");
    goto err_rast;
  }

err_rast:
  nsvgDeleteRasterizer(rast);

err_image:
  nsvgDelete(nvi);

err:
  return image->tex != NULL;
}

void overlayFreeImage(OverlayImage * image)
{
  if (!image->tex)
    return;

  RENDERER(freeTexture, image->tex);
}
