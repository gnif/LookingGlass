#include "UIHelpers.h"
#include <CDebug.h>

WidgetPositioner::~WidgetPositioner()
{
  if (!EndDeferWindowPos(hdwp))
    DEBUG_ERROR_HR(GetLastError(), "EndDeferWindowPos");
}

void WidgetPositioner::move(HWND child, int x, int y, int cx, int cy)
{
  HDWP next = DeferWindowPos(hdwp, child, nullptr, x, y, cx, cy, SWP_NOACTIVATE | SWP_NOZORDER);
  if (next)
    hdwp = next;
  else
    DEBUG_ERROR_HR(GetLastError(), "DeferWindowPos");
}
