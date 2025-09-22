#pragma once
#include <cmath>
#include <windows.h>

struct FontTraits
{
  typedef HFONT Type;
  inline static bool Close(Type h) { return DeleteObject(h); }
  inline static Type GetInvalidValue() { return nullptr; }
};

class WidgetPositioner
{
  HDWP hdwp;
  double m_scale;
  DWORD width, height;
  inline int scale(int value) { return (int)std::round(value * m_scale); }

public:
  WidgetPositioner(double scale, DWORD width, DWORD height) :
    m_scale(scale), width(width), height(height),
    hdwp(BeginDeferWindowPos(10)) {}

  ~WidgetPositioner();

  void move(HWND child, int x, int y, int cx, int cy);

  void pinTopLeft(HWND child, int x, int y, int cx, int cy)
  {
    move(child, scale(x), scale(y), scale(cx), scale(cy));
  }

  void pinTopRight(HWND child, int x, int y, int cx, int cy)
  {
    move(child, scale(x), scale(y), width - scale(cx), scale(cy));
  }

  void pinTopLeftRight(HWND child, int x, int y, int rx, int cy)
  {
    move(child, scale(x), scale(y), width - scale(rx + x), scale(cy));
  }

  void pinLeftTopBottom(HWND child, int x, int y, int cx, int by)
  {
    move(child, scale(x), scale(y), scale(cx), height - scale(y + by));
  }
};
