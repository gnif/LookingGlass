#include "CWindow.h"
#include <strsafe.h>
#include <CDebug.h>

ATOM CWindow::s_atom = 0;
static HINSTANCE hInstance = (HINSTANCE)GetModuleHandle(NULL);

bool CWindow::registerClass()
{
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

CWindow::CWindow()
{
  CreateWindowEx(0, MAKEINTATOM(s_atom), NULL,
    0, 0, 0, 0, 0, NULL, NULL, hInstance, this);
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
  return DefWindowProc(m_hwnd, uMsg, wParam, lParam);  
}

CWindow::~CWindow()
{
  if (m_hwnd)
    DestroyWindow(m_hwnd);
}
