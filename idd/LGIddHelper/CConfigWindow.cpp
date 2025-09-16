#include "CConfigWindow.h"
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
  if (!CreateWindowEx(0, MAKEINTATOM(s_atom), L"Looking Glass IDD Configuration",
    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 500, 400,
    NULL, NULL, hInstance, this))
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create window");
  }

  m_scale = GetDpiForWindow(m_hwnd) / 96.0;
  m_version.reset(new CStaticWidget(L"Looking Glass IDD " LG_VERSION_STR, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, m_hwnd));
  updateFont();
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

  for (HWND child : std::initializer_list<HWND>({ *m_version }))
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
  default:
    return CWindow::handleMessage(uMsg, wParam, lParam);
  }
}

LRESULT CConfigWindow::onCreate()
{
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
  return 0;
}
