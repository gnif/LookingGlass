#include "common/dpi.h"
#include "common/windebug.h"

typedef enum MONITOR_DPI_TYPE {
  MDT_EFFECTIVE_DPI,
  MDT_ANGULAR_DPI,
  MDT_RAW_DPI,
  MDT_DEFAULT
} MONITOR_DPI_TYPE;
typedef HRESULT (WINAPI *GetDpiForMonitor_t)(HMONITOR hmonitor, MONITOR_DPI_TYPE dpiType, UINT * dpiX, UINT * dpiY);

UINT monitor_dpi(HMONITOR hMonitor)
{
  HMODULE shcore = LoadLibraryA("shcore.dll");
  if (!shcore)
  {
    DEBUG_ERROR("Could not load shcore.dll");
    return DPI_100_PERCENT;
  }

  GetDpiForMonitor_t GetDpiForMonitor = (GetDpiForMonitor_t) GetProcAddress(shcore, "GetDpiForMonitor");
  if (!GetDpiForMonitor)
  {
    DEBUG_ERROR("Could not find GetDpiForMonitor");
    return DPI_100_PERCENT;
  }

  UINT dpiX, dpiY;
  HRESULT status = GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
  if (FAILED(status))
  {
    DEBUG_WINERROR("GetDpiForMonitor failed", status);
    return DPI_100_PERCENT;
  }

  return dpiX;
}
