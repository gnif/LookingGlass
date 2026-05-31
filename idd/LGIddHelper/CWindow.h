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

﻿#pragma once

#include <windows.h>
#include <shellapi.h>

class CWindow {
  static LRESULT CALLBACK wndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

protected:
  virtual LRESULT onCreate();
  virtual LRESULT onClose();
  virtual LRESULT onDestroy();
  virtual LRESULT onFinal();

  static HINSTANCE hInstance;
  static void populateWindowClass(WNDCLASSEX &wx);

  virtual LRESULT handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);

  HWND m_hwnd = NULL;

public:
  virtual ~CWindow();
  void destroy();

  HWND hwnd() { return m_hwnd; }
  operator HWND() { return m_hwnd; }
};
