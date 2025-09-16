#include "CNotifyWindow.h"
#include "CConfigWindow.h"
#include <CDebug.h>
#include <windowsx.h>
#include <strsafe.h>

#define WM_NOTIFY_ICON (WM_USER)
#define WM_CLEAN_UP_CONFIG (WM_USER+1)

#define ID_MENU_SHOW_LOG 3000
#define ID_MENU_SHOW_CONFIG 3001

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
    AppendMenu(m_menu, MF_STRING, ID_MENU_SHOW_CONFIG, L"Open configuration");
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
  case WM_NOTIFY_ICON:
    return onNotifyIcon(LOWORD(lParam), HIWORD(lParam), GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam));
  case WM_CLEAN_UP_CONFIG:
    if (m_config && !m_config->hwnd())
    {
      DEBUG_INFO("Config window closed");
      m_config.reset();
    }
    return 0;
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

LRESULT CNotifyWindow::onFinal()
{
  PostQuitMessage(0);
  return CWindow::onFinal();
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
    case ID_MENU_SHOW_CONFIG:
      DEBUG_INFO("Config window opened");
      m_config.reset(new CConfigWindow());
      m_config->onDestroy([this]() {
        PostMessage(m_hwnd, WM_CLEAN_UP_CONFIG, 0, 0);
      });
      ShowWindow(*m_config, SW_NORMAL);
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
