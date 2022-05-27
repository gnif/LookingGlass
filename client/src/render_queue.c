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

#include "render_queue.h"

#include <string.h>

#include "common/ll.h"
#include "main.h"
#include "overlays.h"

struct ll * l_renderQueue = NULL;

void renderQueue_init(void)
{
  l_renderQueue = ll_new();
}

void renderQueue_free(void)
{
  if (!l_renderQueue)
    return;

  renderQueue_clear();
  ll_free(l_renderQueue);
}

void renderQueue_clear(void)
{
  RenderCommand * cmd;
  while(ll_shift(l_renderQueue, (void **)&cmd))
  {
    if (cmd->op == SPICE_OP_DRAW_BITMAP)
      free(cmd->spiceDrawBitmap.data);
    free(cmd);
  }
}

void renderQueue_spiceConfigure(int width, int height)
{
  RenderCommand * cmd = malloc(sizeof(*cmd));
  cmd->op                    = SPICE_OP_CONFIGURE;
  cmd->spiceConfigure.width  = width;
  cmd->spiceConfigure.height = height;
  ll_push(l_renderQueue, cmd);
  app_invalidateWindow(true);
}

void renderQueue_spiceDrawFill(int x, int y, int width, int height,
    uint32_t color)
{
  RenderCommand * cmd = malloc(sizeof(*cmd));
  cmd->op                   = SPICE_OP_DRAW_FILL;
  cmd->spiceFillRect.x      = x;
  cmd->spiceFillRect.y      = y;
  cmd->spiceFillRect.width  = width;
  cmd->spiceFillRect.height = height;
  cmd->spiceFillRect.color  = color;
  ll_push(l_renderQueue, cmd);
  app_invalidateWindow(true);
}

void renderQueue_spiceDrawBitmap(int x, int y, int width, int height, int stride,
    void * data, bool topDown)
{
  RenderCommand * cmd = malloc(sizeof(*cmd));
  cmd->op                      = SPICE_OP_DRAW_BITMAP;
  cmd->spiceDrawBitmap.x       = x;
  cmd->spiceDrawBitmap.y       = y;
  cmd->spiceDrawBitmap.width   = width;
  cmd->spiceDrawBitmap.height  = height;
  cmd->spiceDrawBitmap.stride  = stride;
  cmd->spiceDrawBitmap.data    = malloc(height * stride);
  cmd->spiceDrawBitmap.topDown = topDown;
  memcpy(cmd->spiceDrawBitmap.data, data, height * stride);
  ll_push(l_renderQueue, cmd);
  app_invalidateWindow(true);
}

void renderQueue_spiceShow(bool show)
{
  RenderCommand * cmd = malloc(sizeof(*cmd));
  cmd->op             = SPICE_OP_SHOW;
  cmd->spiceShow.show = show;
  ll_push(l_renderQueue, cmd);
  app_invalidateWindow(true);
}

void renderQueue_process(void)
{
  RenderCommand * cmd;
  while(ll_shift(l_renderQueue, (void **)&cmd))
  {
    switch(cmd->op)
    {
      case SPICE_OP_CONFIGURE:
        RENDERER(spiceConfigure,
            cmd->spiceConfigure.width, cmd->spiceConfigure.height);
        break;

      case SPICE_OP_DRAW_FILL:
        RENDERER(spiceDrawFill,
            cmd->spiceFillRect.x    , cmd->spiceFillRect.y,
            cmd->spiceFillRect.width, cmd->spiceFillRect.height,
            cmd->spiceFillRect.color);
        break;

      case SPICE_OP_DRAW_BITMAP:
        RENDERER(spiceDrawBitmap,
            cmd->spiceDrawBitmap.x     , cmd->spiceDrawBitmap.y,
            cmd->spiceDrawBitmap.width , cmd->spiceDrawBitmap.height,
            cmd->spiceDrawBitmap.stride, cmd->spiceDrawBitmap.data,
            cmd->spiceDrawBitmap.topDown);
        free(cmd->spiceDrawBitmap.data);
        break;

      case SPICE_OP_SHOW:
        RENDERER(spiceShow, cmd->spiceShow.show);
        if (cmd->spiceShow.show)
          overlaySplash_show(false);
        break;
    }
    free(cmd);
  }
}
