#include "CWindow.h"
#include <windowsx.h>
#include <strsafe.h>
#include <CDebug.h>

#define ID_MENU_SHOW_LOG 3000

ATOM CWindow::s_atom = 0;
UINT CWindow::s_taskbarCreated = 0;
static HINSTANCE hInstance = (HINSTANCE)GetModuleHandle(NULL);

bool CWindow::registerClass()
{
  s_taskbarCreated = RegisterWindowMessage(L"TaskbarCreated");
  if (!s_taskbarCreated)
    DEBUG_WARN_HR(GetLastError(), "RegisterWindowMessage(TaskbarCreated)");

  WNDCLASSEX wx = {};
  wx.cbSize = sizeof(WNDCLASSEX);
  wx.lpfnWndProc = wndProc;
  wx.hInstance = hInstance;
  wx.lpszClassName = L"LookingGlassIddHelper";
  wx.hIcon = LoadIcon(NULL, IDI_APPLICATION);
  wx.hIconSm = LoadIcon(NULL, IDI_APPLICATION);
  wx.hCursor = LoadCursor(NULL, IDC_ARROW);
  wx.hbrBackground = (HBRUSH)COLOR_APPWORKSPACE;

  s_atom = RegisterClassEx(&wx);
  return s_atom;
}

CWindow::CWindow() : m_iconData({ 0 }), m_menu(CreatePopupMenu())
{
  CreateWindowEx(0, MAKEINTATOM(s_atom), NULL,
    0, 0, 0, 0, 0, NULL, NULL, hInstance, this);

  if (m_menu)
  {
    AppendMenu(m_menu, MF_STRING, ID_MENU_SHOW_LOG, L"Open log directory");
  }
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
    return DefWindowProc(m_hwnd, uMsg, wParam, lParam);
  }
}

LRESULT CWindow::onCreate()
{
  registerIcon();
  return 0;
}

LRESULT CWindow::onNotifyIcon(UINT uEvent, WORD wIconId, int x, int y)
{
  switch (uEvent)
  {
  case WM_CONTEXTMENU:
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

void CWindow::registerIcon()
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

void CWindow::destroy()
{
  if (m_hwnd)
  {
    DestroyWindow(m_hwnd);
    m_hwnd = NULL;
  }
}

CWindow::~CWindow()
{
  destroy();
  DestroyMenu(m_menu);
}
