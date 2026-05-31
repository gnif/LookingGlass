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

﻿#include "CWidget.h"

#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

HWND CWidget::createWindowSimple(LPCWSTR cls, LPCWSTR title, DWORD style, HWND parent, DWORD dwExStyle)
{
  return CreateWindowEx(dwExStyle, cls, title, style, 0, 0, 0, 0, parent,
    NULL, (HINSTANCE)GetModuleHandle(NULL), NULL);
}

CWidget::~CWidget()
{
  destroy();
}

void CWidget::destroy()
{
  if (m_hwnd)
  {
    DestroyWindow(m_hwnd);
    m_hwnd = NULL;
  }
}
