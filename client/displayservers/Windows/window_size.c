/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
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

#include "window_size.h"

void windowsWindowSizeInit(WindowsWindowSize * state,
    int width, int height)
{
  state->width   = width;
  state->height  = height;
  state->pending = false;
}

bool windowsWindowSizeRequest(WindowsWindowSize * state,
    int width, int height, bool defer)
{
  state->width   = width;
  state->height  = height;
  state->pending = defer;
  return !defer;
}

bool windowsWindowSizeTakePending(WindowsWindowSize * state,
    bool defer, int * width, int * height)
{
  if (!state->pending || defer)
    return false;

  *width          = state->width;
  *height         = state->height;
  state->pending  = false;
  return true;
}
