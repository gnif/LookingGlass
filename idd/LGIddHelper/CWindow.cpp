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

#include "CWindow.h"
#include <windowsx.h>
#include <strsafe.h>
#include <CDebug.h>

HINSTANCE CWindow::hInstance = (HINSTANCE)GetModuleHandle(NULL);

void CWindow::populateWindowClass(WNDCLASSEX &wx)
{
  wx.cbSize = sizeof(WNDCLASSEX);
  wx.lpfnWndProc = wndProc;
  wx.hInstance = hInstance;
  wx.hIcon = LoadIcon(NULL, IDI_APPLICATION);
  wx.hIconSm = LoadIcon(NULL, IDI_APPLICATION);
  wx.hCursor = LoadCursor(NULL, IDC_ARROW);
  wx.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
}

LRESULT CWindow::wndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  CWindow *self;
  if (uMsg == WM_NCCREATE)
  {
    LPCREATESTRUCT lpcs = (LPCREATESTRUCT)lParam;
    self = (CWindow*)lpcs->lpCreateParams;
    self->m_hwnd = hwnd;
    SetWindowLongPtr(hwnd, GWLP_USERDATA, (LPARAM) self);
  }
  else
  {
    self = (CWindow*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
  }

  if (self)
    return self->handleMessage(uMsg, wParam, lParam);
  else
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

LRESULT CWindow::handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  switch (uMsg)
  {
  case WM_CREATE:
    return onCreate();
  case WM_CLOSE:
    return onClose();
  case WM_DESTROY:
    return onDestroy();
  case WM_NCDESTROY:
    return onFinal();
  default:
    return DefWindowProc(m_hwnd, uMsg, wParam, lParam);
  }
}

LRESULT CWindow::onCreate()
{
  return 0;
}

LRESULT CWindow::onClose()
{
  return DefWindowProc(m_hwnd, WM_CLOSE, 0, 0);
}

LRESULT CWindow::onDestroy()
{
  return 0;
}

LRESULT CWindow::onFinal()
{
  m_hwnd = 0;
  return 0;
}

void CWindow::destroy()
{
  if (m_hwnd)
    DestroyWindow(m_hwnd);
}

CWindow::~CWindow()
{
  destroy();
}
