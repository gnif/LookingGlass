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
  wx.hbrBackground = (HBRUSH)COLOR_APPWORKSPACE;
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
  default:
    return DefWindowProc(m_hwnd, uMsg, wParam, lParam);
  }
}

LRESULT CWindow::onCreate()
{
  return 0;
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
}
