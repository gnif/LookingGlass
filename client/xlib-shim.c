#include <X11/Xlib.h>
#include <stdbool.h>
#include "debug.h"

int XSync(Display * display, Bool discard)
{
  static bool doneInfo = false;
  if (!doneInfo)
  {
    DEBUG_INFO("XSync Override Enabled");
    doneInfo = true;
  }

  return 0;
}