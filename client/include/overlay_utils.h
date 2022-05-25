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

#ifndef _H_LG_OVERLAY_UTILS_
#define _H_LG_OVERLAY_UTILS_

#include <stdbool.h>

#include "common/types.h"

typedef struct ImVec2 ImVec2;

typedef struct
{
  void * tex;
  int    width;
  int    height;
}
OverlayImage;

void overlayGetImGuiRect(struct Rect * rect);
ImVec2 * overlayGetScreenSize(void);
void overlayTextURL(const char * url, const char * text);
void overlayTextMaybeURL(const char * text, bool wrapped);

// create a texture from a SVG and scale it to fit the supplied width & height
bool overlayLoadSVG(const char * data, unsigned int size, OverlayImage * image,
    int width, int height);

void overlayFreeImage(OverlayImage * image);

#endif
