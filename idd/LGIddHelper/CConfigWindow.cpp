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

#include "CConfigWindow.h"
#include "CListBox.h"
#include "CGroupBox.h"
#include "CEditWidget.h"
#include "CButton.h"
#include "CCheckbox.h"
#include <CDebug.h>
#include <windowsx.h>
#include <strsafe.h>
#include "VersionInfo.h"

ATOM CConfigWindow::s_atom = 0;

bool CConfigWindow::registerClass()
{
  WNDCLASSEX wx = {};
  populateWindowClass(wx);
  wx.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
  wx.lpszClassName = L"LookingGlassIddConfig";

  s_atom = RegisterClassEx(&wx);
  return s_atom;
}

CConfigWindow::CConfigWindow() : m_scale(1)
{
  LSTATUS error = m_settings.open();
  if (error != ERROR_SUCCESS)
    DEBUG_ERROR_HR(error, "Failed to load settings");
  else
  {
    m_modes = m_settings.getModes();
    m_defaultRefresh = m_settings.getDefaultRefresh();
    m_noGPU = m_settings.getNoGPU();
    m_exclusive = m_settings.getExclusiveMonitor();
  }

  if (!CreateWindowEx(0, MAKEINTATOM(s_atom), L"Looking Glass IDD Configuration",
    WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
    NULL, NULL, hInstance, this))
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create window");
  }
}

void CConfigWindow::getMinimumSize(LONG &width, LONG &height)
{
  RECT client = { 0, 0, (LONG)(436 * m_scale), (LONG)(340 * m_scale) };
  AdjustWindowRect(&client, WS_OVERLAPPEDWINDOW, FALSE);
  width = client.right - client.left;
  height = client.bottom - client.top;
}

void CConfigWindow::updateFont()
{
  NONCLIENTMETRICS ncmMetrics = { sizeof(NONCLIENTMETRICS) };
  UINT dpi = GetDpiForWindow(m_hwnd);
  if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncmMetrics, &ncmMetrics, 0, dpi))
  {
    DEBUG_ERROR_HR(GetLastError(), "SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS)");
    return;
  }

  m_font.Attach(CreateFontIndirect(&ncmMetrics.lfMessageFont));
  if (!m_font.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "CreateFontIndirect(lfMessageFont)");
    return;
  }

  for (HWND child : std::initializer_list<HWND>({
    *m_version, *m_modeGroup, *m_modeBox, *m_widthLabel, *m_heightLabel, *m_refreshLabel, *m_modePreferred,
    *m_modeWidth, *m_modeHeight, *m_modeRefresh, *m_modeUpdate, *m_modeDelete, *m_modeReset,
    *m_defRefreshLabel, *m_defRefresh, *m_defRefreshHz, *m_modeSave, *m_modeRevert,
    *m_prefGroup, *m_prefNoGPU, *m_prefExclusive,
  }))
    SendMessage(child, WM_SETFONT, (WPARAM)m_font.Get(), 1);
}

int CConfigWindow::updateModeList(int wanted)
{
  int result = 0;
  m_modeBox->addItem(L"<add new>", -1);

  auto &modes = *m_modes;
  for (size_t i = 0; i < modes.size(); ++i)
  {
    int idx = m_modeBox->addItem(modes[i].toString(), i);
    if (wanted == i)
      result = idx;
  }

  return result;
}

LRESULT CConfigWindow::handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  switch (uMsg)
  {
  case WM_SIZE:
    return onResize(LOWORD(lParam), HIWORD(lParam));
  case WM_DPICHANGED:
  {
    LPRECT lpBox = (LPRECT)lParam;
    m_scale = LOWORD(wParam) / 96.0;
    updateFont();
    SetWindowPos(m_hwnd, NULL, lpBox->left, lpBox->top, lpBox->right - lpBox->left,
      lpBox->bottom - lpBox->top, SWP_NOZORDER | SWP_NOACTIVATE);
    onResize(lpBox->right - lpBox->left, lpBox->bottom - lpBox->top);
    RedrawWindow(m_hwnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
    return 0;
  }
  case WM_GETMINMAXINFO:
  {
    LPMINMAXINFO lpMmi = (LPMINMAXINFO)lParam;
    getMinimumSize(lpMmi->ptMinTrackSize.x, lpMmi->ptMinTrackSize.y);
    return 0;
  }
  case WM_COMMAND:
    return onCommand(LOWORD(wParam), HIWORD(wParam), (HWND)lParam);
  default:
    return CWindow::handleMessage(uMsg, wParam, lParam);
  }
}

LRESULT CConfigWindow::onCreate()
{
  m_scale = GetDpiForWindow(m_hwnd) / 96.0;
  m_version.reset(new CStaticWidget(L"Looking Glass IDD " LG_VERSION_STR, SS_CENTERIMAGE, m_hwnd));

  m_modeGroup.reset(new CGroupBox(L"Driver mode settings", 0, m_hwnd));

  m_modeBox.reset(new CListBox(WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY, m_hwnd));
  if (m_modes)
    updateModeList();

  m_widthLabel.reset(new CStaticWidget(L"Width:", SS_CENTERIMAGE, m_hwnd));
  m_heightLabel.reset(new CStaticWidget(L"Height:", SS_CENTERIMAGE, m_hwnd));
  m_refreshLabel.reset(new CStaticWidget(L"Refresh:", SS_CENTERIMAGE, m_hwnd));

  m_modeWidth.reset(new CEditWidget(WS_TABSTOP | ES_LEFT | ES_NUMBER, m_hwnd));
  m_modeHeight.reset(new CEditWidget(WS_TABSTOP | ES_LEFT | ES_NUMBER, m_hwnd));
  m_modeRefresh.reset(new CEditWidget(WS_TABSTOP | ES_LEFT | ES_NUMBER, m_hwnd));
  m_modePreferred.reset(new CCheckbox(L"prefer", 0, m_hwnd));

  m_modeUpdate.reset(new CButton(L"Update", WS_TABSTOP, m_hwnd));
  m_modeDelete.reset(new CButton(L"Delete", WS_TABSTOP, m_hwnd));
  m_modeReset.reset(new CButton(L"Load default", WS_TABSTOP, m_hwnd));
  EnableWindow(*m_modeUpdate, FALSE);
  EnableWindow(*m_modeDelete, FALSE);

  m_modeSave.reset(new CButton(L"Save && reload driver", WS_TABSTOP, m_hwnd));
  m_modeRevert.reset(new CButton(L"Revert", WS_TABSTOP, m_hwnd));

  m_defRefreshLabel.reset(new CStaticWidget(L"Default refresh:", SS_CENTERIMAGE, m_hwnd));
  m_defRefresh.reset(new CEditWidget(ES_LEFT | ES_NUMBER | WS_TABSTOP, m_hwnd));
  m_defRefreshHz.reset(new CStaticWidget(L"Hz", SS_CENTERIMAGE, m_hwnd));

  if (m_defaultRefresh)
    m_defRefresh->setNumericValue(*m_defaultRefresh);
  else
    m_defRefresh->disable();

  m_prefGroup.reset(new CGroupBox(L"Preferences", 0, m_hwnd));
  m_prefNoGPU.reset(new CCheckbox(L"Disable no GPU warning", 0, m_hwnd));
  m_prefExclusive.reset(new CCheckbox(L"Make LG the only monitor", 0, m_hwnd));

  if (m_noGPU)
    m_prefNoGPU->setChecked(*m_noGPU);

  if (m_exclusive)
    m_prefExclusive->setChecked(*m_exclusive);

  LONG width, height;
  getMinimumSize(width, height);
  SetWindowPos(m_hwnd, NULL, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER);

  updateFont();

  return 0;
}

LRESULT CConfigWindow::onFinal()
{
  if (m_onDestroy)
    m_onDestroy();
  return CWindow::onFinal();
}

LRESULT CConfigWindow::onResize(DWORD width, DWORD height)
{
  WidgetPositioner pos(m_scale, width, height);
  pos.pinTopLeftRight(*m_version, 12, 12, 12, 20);

  pos.pinLeftTopBottom(*m_modeGroup, 12, 40, 200, 12);
  pos.pinTopLeft(*m_defRefreshLabel, 24, 64, 95, 20);
  pos.pinTopLeft(*m_defRefresh, 119, 64, 63, 20);
  pos.pinTopLeft(*m_defRefreshHz, 186, 64, 16, 20);
  pos.pinLeftTopBottom(*m_modeBox, 24, 90, 176, 148);
  pos.pinBottomLeft(*m_widthLabel, 24, 124, 50, 20);
  pos.pinBottomLeft(*m_heightLabel, 24, 100, 50, 20);
  pos.pinBottomLeft(*m_refreshLabel, 24, 76, 50, 20);
  pos.pinBottomLeft(*m_modeWidth, 75, 124, 50, 20);
  pos.pinBottomLeft(*m_modeHeight, 75, 100, 50, 20);
  pos.pinBottomLeft(*m_modeRefresh, 75, 76, 50, 20);
  pos.pinBottomLeft(*m_modePreferred, 130, 124, 70, 20);
  pos.pinBottomLeft(*m_modeUpdate, 20, 48, 50, 24);
  pos.pinBottomLeft(*m_modeDelete, 72, 48, 50, 24);
  pos.pinBottomLeft(*m_modeReset, 122, 48, 82, 24);
  pos.pinBottomLeft(*m_modeSave, 20, 20, 132, 24);
  pos.pinBottomLeft(*m_modeRevert, 154, 20, 50, 24);

  pos.pinTopLeft(*m_prefGroup, 224, 40, 200, 72);
  pos.pinTopLeft(*m_prefNoGPU, 236, 64, 176, 20);
  pos.pinTopLeft(*m_prefExclusive, 236, 84, 176, 20);
  return 0;
}

void CConfigWindow::onModeListSelectChange()
{
  int sel = m_modeBox->getSel();
  if (sel == LB_ERR)
  {
    EnableWindow(*m_modeUpdate, FALSE);
    EnableWindow(*m_modeDelete, FALSE);
    return;
  }

  int index = m_modeBox->getData(sel);

  if (index >= 0)
  {
    auto &mode = (*m_modes)[index];
    m_modeWidth->setNumericValue(mode.width);
    m_modeHeight->setNumericValue(mode.height);
    m_modeRefresh->setNumericValue(mode.refresh);
    m_modePreferred->setChecked(mode.preferred);
  }
  EnableWindow(*m_modeUpdate, TRUE);
  EnableWindow(*m_modeDelete, index >= 0);
}

LRESULT CConfigWindow::onCommand(WORD id, WORD code, HWND hwnd)
{
  if (m_modeBox && hwnd == *m_modeBox && code == LBN_SELCHANGE && m_modes)
  {
    onModeListSelectChange();
  }
  else if (m_modePreferred && hwnd == *m_modePreferred && code == BN_CLICKED && m_modes)
  {
    m_modePreferred->setChecked(!m_modePreferred->isChecked());
  }
  else if (m_modeUpdate && hwnd == *m_modeUpdate && code == BN_CLICKED && m_modes)
  {
    int sel = m_modeBox->getSel();
    if (sel == LB_ERR)
      return 0;

    int index = m_modeBox->getData(sel);
    auto &mode = index >= 0 ? (*m_modes)[index] : m_modes->emplace_back();

    for (auto &mode : *m_modes)
      mode.preferred = false;

    try
    {
      mode.width = m_modeWidth->getNumericValue();
      mode.height = m_modeHeight->getNumericValue();
      mode.refresh = m_modeRefresh->getNumericValue();
      mode.preferred = m_modePreferred->isChecked();
    }
    catch (std::logic_error&)
    {
      return 0;
    }

    m_modeBox->clear();
    m_modeBox->setSel(updateModeList(index));
  }
  else if (m_modeDelete && hwnd == *m_modeDelete && code == BN_CLICKED && m_modes)
  {
    int sel = m_modeBox->getSel();
    if (sel == LB_ERR)
      return 0;

    int index = m_modeBox->getData(sel);
    m_modeBox->clear();
    m_modes->erase(m_modes->begin() + index);

    updateModeList();
    onModeListSelectChange();
  }
  else if (m_modeReset && hwnd == *m_modeReset && code == BN_CLICKED && m_modes)
  {
    *m_modes = m_settings.getDefaultModes();
    m_modeBox->clear();
    updateModeList();
    onModeListSelectChange();
  }
  else if (m_defRefresh && hwnd == *m_defRefresh && code == EN_CHANGE && m_defaultRefresh)
  {
    try
    {
      m_defaultRefresh = m_defRefresh->getNumericValue();
    }
    catch (std::logic_error &)
    {
      return 0;
    }
  }
  else if (m_prefNoGPU && hwnd == *m_prefNoGPU && code == BN_CLICKED && m_noGPU)
  {
    *m_noGPU ^= true;
    m_settings.setNoGPU(*m_noGPU);
    m_prefNoGPU->setChecked(*m_noGPU);
  }
  else if (m_prefExclusive && hwnd == *m_prefExclusive && code == BN_CLICKED && m_exclusive)
  {
    *m_exclusive ^= true;
    m_settings.setExclusiveMonitor(*m_exclusive);
    m_prefExclusive->setChecked(*m_exclusive);
  }
  else if (m_modeSave && hwnd == *m_modeSave && code == BN_CLICKED)
  {
    bool updated = false;

    if (m_modes)
    {
      LRESULT result = m_settings.setModes(*m_modes);
      if (result == ERROR_SUCCESS)
        updated = true;
      else
        DEBUG_ERROR_HR((HRESULT)result, "Failed to save modes");
    }

    if (m_defaultRefresh)
    {
      LRESULT result = m_settings.setDefaultRefresh(*m_defaultRefresh);
      if (result == ERROR_SUCCESS)
        updated = true;
      else
        DEBUG_ERROR_HR((HRESULT)result, "Failed to default refresh");
    }

    if (updated)
      sendSettingChange();
  }
  else if (m_modeRevert && hwnd == *m_modeRevert && code == BN_CLICKED)
  {
    m_modes = m_settings.getModes();
    m_defaultRefresh = m_settings.getDefaultRefresh();

    if (m_modes)
    {
      m_modeBox->clear();
      updateModeList();
    }

    if (m_defaultRefresh)
      m_defRefresh->setNumericValue(*m_defaultRefresh);
    else
      m_defRefresh->disable();
  }
  return 0;
}
