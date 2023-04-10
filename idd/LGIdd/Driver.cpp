#include "driver.h"
#include "driver.tmh"

#include "CPlatformInfo.h"

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT  DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
  WDF_DRIVER_CONFIG config;
  NTSTATUS status;
  WDF_OBJECT_ATTRIBUTES attributes;

#if UMDF_VERSION_MAJOR == 2 && UMDF_VERSION_MINOR == 0
  WPP_INIT_TRACING(MYDRIVER_TRACING_ID);
#else
  WPP_INIT_TRACING(DriverObject, RegistryPath);
#endif

  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

  WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
  attributes.EvtCleanupCallback = LGIddEvtDriverContextCleanup;
  WDF_DRIVER_CONFIG_INIT(&config, LGIddEvtDeviceAdd);

  CPlatformInfo::Init();
  status = WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config, WDF_NO_HANDLE);
  if (!NT_SUCCESS(status))
  {
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "WdfDriverCreate failed %!STATUS!", status);
#if UMDF_VERSION_MAJOR == 2 && UMDF_VERSION_MINOR == 0
    WPP_CLEANUP();
#else
    WPP_CLEANUP(DriverObject);
#endif
    return status;
  }

  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
  return status;
}

NTSTATUS LGIddEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
  NTSTATUS status;
  UNREFERENCED_PARAMETER(Driver);

  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");
  status = LGIddCreateDevice(DeviceInit);
  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
  return status;
}

VOID LGIddEvtDriverContextCleanup(_In_ WDFOBJECT DriverObject)
{
  UNREFERENCED_PARAMETER(DriverObject);

  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

#if UMDF_VERSION_MAJOR == 2 && UMDF_VERSION_MINOR == 0
  WPP_CLEANUP();
#else
  WPP_CLEANUP(WdfDriverWdmGetDriverObject((WDFDRIVER)DriverObject));
#endif
}