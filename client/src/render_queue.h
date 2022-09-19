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

#include "common/ll.h"

typedef struct
{
  enum
  {
    SPICE_OP_CONFIGURE,
    SPICE_OP_DRAW_FILL,
    SPICE_OP_DRAW_BITMAP,
    SPICE_OP_SHOW,
    CURSOR_OP_STATE,
    CURSOR_OP_IMAGE,
  }
  op;

  union
  {
    struct
    {
      int width, height;
    }
    spiceConfigure;

    struct
    {
      int      x, y;
      int      width, height;
      uint32_t color;
    }
    spiceFillRect;

    struct
    {
      int       x    , y;
      int       width, height;
      int       stride;
      uint8_t * data;
      bool      topDown;
    }
    spiceDrawBitmap;

    struct
    {
      bool show;
    }
    spiceShow;

    struct
    {
      bool visible;
      int  x;
      int  y;
      int  hx;
      int  hy;
    }
    cursorState;

    struct
    {
      bool      monochrome;
      int       width;
      int       height;
      int       pitch;
      uint8_t * data;
    }
    cursorImage;
  };
}
RenderCommand;

void renderQueue_init(void);
void renderQueue_free(void);
void renderQueue_clear(void);
void renderQueue_process(void);

void renderQueue_spiceConfigure(int width, int height);

void renderQueue_spiceDrawFill(int x, int y, int width, int height,
    uint32_t color);

void renderQueue_spiceDrawBitmap(int x, int y, int width, int height, int stride,
    void * data, bool topDown);

void renderQueue_spiceShow(bool show);

void renderQueue_cursorState(bool visible, int x, int y, int hx, int hy);

void renderQueue_cursorImage(bool monochrome, int width, int height, int pitch,
    uint8_t * data);
