﻿#include "CConfigWindow.h"
#include "CListBox.h"
#include "CGroupBox.h"
#include "CEditWidget.h"
#include "CButton.h"
#include <CDebug.h>
#include <windowsx.h>
#include <strsafe.h>
#include "VersionInfo.h"

ATOM CConfigWindow::s_atom = 0;

bool CConfigWindow::registerClass()
{
  WNDCLASSEX wx = {};
  populateWindowClass(wx);
  wx.hIconSm = wx.hIcon = LoadIcon(hInstance, IDI_APPLICATION);
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
    m_modes = m_settings.getModes();

  if (!CreateWindowEx(0, MAKEINTATOM(s_atom), L"Looking Glass IDD Configuration",
    WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 500, 400,
    NULL, NULL, hInstance, this))
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create window");
  }
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
    *m_version, *m_modeGroup, *m_modeBox, *m_widthLabel, *m_heightLabel, *m_refreshLabel,
    *m_modeWidth, *m_modeHeight, *m_modeRefresh, *m_modeUpdate,
  }))
    SendMessage(child, WM_SETFONT, (WPARAM)m_font.Get(), 1);
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
  case WM_COMMAND:
    return onCommand(LOWORD(wParam), HIWORD(wParam), (HWND)lParam);
  default:
    return CWindow::handleMessage(uMsg, wParam, lParam);
  }
}

LRESULT CConfigWindow::onCreate()
{
  m_scale = GetDpiForWindow(m_hwnd) / 96.0;
  m_version.reset(new CStaticWidget(L"Looking Glass IDD " LG_VERSION_STR, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, m_hwnd));

  m_modeGroup.reset(new CGroupBox(L"Custom modes", WS_CHILD | WS_VISIBLE, m_hwnd));

  m_modeBox.reset(new CListBox(WS_CHILD | WS_VISIBLE | LBS_NOTIFY, m_hwnd));
  if (m_modes)
  {
    m_modeBox->addItem(L"<add new>", -1);

    auto &modes = *m_modes;
    for (size_t i = 0; i < modes.size(); ++i)
      m_modeBox->addItem(modes[i].toString(), i);
  }

  m_widthLabel.reset(new CStaticWidget(L"Width:", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, m_hwnd));
  m_heightLabel.reset(new CStaticWidget(L"Height:", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, m_hwnd));
  m_refreshLabel.reset(new CStaticWidget(L"Refresh:", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, m_hwnd));

  m_modeWidth.reset(new CEditWidget(WS_CHILD | WS_VISIBLE | ES_LEFT, m_hwnd));
  m_modeHeight.reset(new CEditWidget(WS_CHILD | WS_VISIBLE | ES_LEFT, m_hwnd));
  m_modeRefresh.reset(new CEditWidget(WS_CHILD | WS_VISIBLE | ES_LEFT, m_hwnd));

  m_modeUpdate.reset(new CButton(L"Save", WS_CHILD | WS_VISIBLE, m_hwnd));
  EnableWindow(*m_modeUpdate, FALSE);

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
  pos.pinLeftTopBottom(*m_modeBox, 24, 64, 176, 120);
  pos.pinBottomLeft(*m_widthLabel, 24, 96, 50, 20);
  pos.pinBottomLeft(*m_heightLabel, 24, 72, 50, 20);
  pos.pinBottomLeft(*m_refreshLabel, 24, 48, 50, 20);
  pos.pinBottomLeft(*m_modeWidth, 75, 96, 50, 20);
  pos.pinBottomLeft(*m_modeHeight, 75, 72, 50, 20);
  pos.pinBottomLeft(*m_modeRefresh, 75, 48, 50, 20);
  pos.pinBottomLeft(*m_modeUpdate, 24, 20, 50, 24);
  return 0;
}

LRESULT CConfigWindow::onCommand(WORD id, WORD code, HWND hwnd)
{
  if (hwnd == *m_modeBox && code == LBN_SELCHANGE && m_modes)
  {
    int sel = m_modeBox->getSel();
    if (sel == LB_ERR)
    {
      EnableWindow(*m_modeUpdate, FALSE);
      return 0;
    }

    int index = m_modeBox->getData(sel);

    if (index >= 0)
    {
      auto &mode = (*m_modes)[index];
      m_modeWidth->setNumericValue(mode.width);
      m_modeHeight->setNumericValue(mode.height);
      m_modeRefresh->setNumericValue(mode.refresh);
    }
    else
    {
      m_modeWidth->setValue(L"");
      m_modeHeight->setValue(L"");
      m_modeRefresh->setValue(L"");
    }
    EnableWindow(*m_modeUpdate, TRUE);
  }
  else if (hwnd == *m_modeUpdate && code == BN_CLICKED && m_modes)
  {
    int sel = m_modeBox->getSel();
    if (sel == LB_ERR)
      return 0;

    int index = m_modeBox->getData(sel);
    auto &mode = index >= 0 ? (*m_modes)[index] : m_modes->emplace_back();
    mode.width = m_modeWidth->getNumericValue();
    mode.height = m_modeHeight->getNumericValue();
    mode.refresh = m_modeRefresh->getNumericValue();

    if (index >= 0)
      m_modeBox->delItem(sel);

    m_modeBox->setSel(m_modeBox->addItem(mode.toString().c_str(), index));

    LRESULT result = m_settings.setModes(*m_modes);
    if (result != ERROR_SUCCESS)
      DEBUG_ERROR_HR(result, "Failed to save modes");
  }
  return 0;
}
