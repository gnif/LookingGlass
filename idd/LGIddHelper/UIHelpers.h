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

#pragma once
#include <cmath>
#include <windows.h>

struct FontTraits
{
  typedef HFONT Type;
  inline static bool Close(Type h) { return DeleteObject(h); }
  inline static Type GetInvalidValue() { return nullptr; }
};

class WidgetPositioner
{
  HDWP hdwp;
  double m_scale;
  DWORD width, height;
  inline int scale(int value) { return (int)std::round(value * m_scale); }

public:
  WidgetPositioner(double scale, DWORD width, DWORD height) :
    m_scale(scale), width(width), height(height),
    hdwp(BeginDeferWindowPos(10)) {}

  ~WidgetPositioner();

  void move(HWND child, int x, int y, int cx, int cy);

  void pinTopLeft(HWND child, int x, int y, int cx, int cy)
  {
    move(child, scale(x), scale(y), scale(cx), scale(cy));
  }

  void pinTopRight(HWND child, int x, int y, int cx, int cy)
  {
    move(child, scale(x), scale(y), width - scale(cx), scale(cy));
  }

  void pinTopLeftRight(HWND child, int x, int y, int rx, int cy)
  {
    move(child, scale(x), scale(y), width - scale(rx + x), scale(cy));
  }

  void pinLeftTopBottom(HWND child, int x, int y, int cx, int by)
  {
    move(child, scale(x), scale(y), scale(cx), height - scale(y + by));
  }

  void pinBottomLeft(HWND child, int x, int by, int cx, int cy)
  {
    move(child, scale(x), height - scale(by + cy), scale(cx), scale(cy));
  }
};
