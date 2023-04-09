#include <windows.h>
#include <wdf.h>
#include <initguid.h>

#include "device.h"
#include "trace.h"

EXTERN_C_START
DRIVER_INITIALIZE DriverEntry;
EXTERN_C_END

EVT_WDF_DRIVER_DEVICE_ADD LGIddEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP LGIddEvtDriverContextCleanup;