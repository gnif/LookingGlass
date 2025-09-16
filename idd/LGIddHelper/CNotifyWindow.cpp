#include "CNotifyWindow.h"
#include <CDebug.h>
#include <windowsx.h>
#include <strsafe.h>

#define ID_MENU_SHOW_LOG 3000

ATOM CNotifyWindow::s_atom = 0;
UINT CNotifyWindow::s_taskbarCreated = 0;

bool CNotifyWindow::registerClass()
{
  s_taskbarCreated = RegisterWindowMessage(L"TaskbarCreated");
  if (!s_taskbarCreated)
    DEBUG_WARN_HR(GetLastError(), "RegisterWindowMessage(TaskbarCreated)");

  WNDCLASSEX wx = {};
  populateWindowClass(wx);
  wx.lpszClassName = L"LookingGlassIddHelper";

  s_atom = RegisterClassEx(&wx);
  return s_atom;
}

CNotifyWindow::CNotifyWindow() : m_iconData({ 0 }), m_menu(CreatePopupMenu()),
  closeRequested(false)
{
  CreateWindowEx(0, MAKEINTATOM(s_atom), NULL,
    0, 0, 0, 0, 0, NULL, NULL, hInstance, this);

  if (m_menu)
  {
    AppendMenu(m_menu, MF_STRING, ID_MENU_SHOW_LOG, L"Open log directory");
  }
}

CNotifyWindow::~CNotifyWindow()
{
  DestroyMenu(m_menu);
}

LRESULT CNotifyWindow::handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  switch (uMsg)
  {
  case WM_NCDESTROY:
    PostQuitMessage(0);
    return 0;
  case WM_NOTIFY_ICON:
    return onNotifyIcon(LOWORD(lParam), HIWORD(lParam), GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam));
  default:
    if (s_taskbarCreated && uMsg == s_taskbarCreated)
    {
      registerIcon();
      return 0;
    }
    return CWindow::handleMessage(uMsg, wParam, lParam);
  }
}

LRESULT CNotifyWindow::onCreate()
{
  registerIcon();
  return 0;
}

LRESULT CNotifyWindow::onClose()
{
  if (closeRequested)
    destroy();
  return 0;
}

LRESULT CNotifyWindow::onNotifyIcon(UINT uEvent, WORD wIconId, int x, int y)
{
  switch (uEvent)
  {
  case WM_CONTEXTMENU:
    SetForegroundWindow(m_hwnd);
    switch (TrackPopupMenu(m_menu, TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, m_hwnd, NULL))
    {
    case ID_MENU_SHOW_LOG:
      ShellExecute(m_hwnd, L"open", g_debug.logDir(), NULL, NULL, SW_NORMAL);
      break;
    }
    break;
  }
  return 0;
}

void CNotifyWindow::registerIcon()
{
  m_iconData.cbSize = sizeof m_iconData;
  m_iconData.hWnd = m_hwnd;
  m_iconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  m_iconData.uCallbackMessage = WM_NOTIFY_ICON;
  m_iconData.hIcon = LoadIcon(hInstance, IDI_APPLICATION);
  m_iconData.uVersion = NOTIFYICON_VERSION_4;
  StringCbCopy(m_iconData.szTip, sizeof m_iconData.szTip, L"Looking Glass (IDD)");

  if (!Shell_NotifyIcon(NIM_ADD, &m_iconData))
    DEBUG_ERROR_HR(GetLastError(), "Shell_NotifyIcon(NIM_ADD)");

  if (!Shell_NotifyIcon(NIM_SETVERSION, &m_iconData))
    DEBUG_ERROR_HR(GetLastError(), "Shell_NotifyIcon(NIM_SETVERSION)");
}

void CNotifyWindow::close()
{
  closeRequested = true;
  PostMessage(m_hwnd, WM_CLOSE, 0, 0);
}
