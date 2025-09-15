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
  wx.lpszClassName = L"LookingGlassIddConfig";

  s_atom = RegisterClassEx(&wx);
  return s_atom;
}

CConfigWindow::CConfigWindow()
{
  if (!CreateWindowEx(0, MAKEINTATOM(s_atom), L"Looking Glass IDD Configuration",
    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 300, 200,
    NULL, NULL, hInstance, this))
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create window");
  }

  m_version.reset(new CStaticWidget(L"Looking Glass IDD " LG_VERSION_STR, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, m_hwnd));
}

LRESULT CConfigWindow::handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  switch (uMsg)
  {
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
