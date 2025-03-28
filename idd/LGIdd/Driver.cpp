/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "driver.h"
#include "driver.tmh"

#include "CDebug.h"
#include "CPlatformInfo.h"
#include "VersionInfo.h"
#include "CPipeServer.h"

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT  DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
  g_debug.Init("looking-glass-idd");
  DEBUG_INFO("Looking Glass IDD Driver (" LG_VERSION_STR ")");

  NTSTATUS status = STATUS_SUCCESS;
#if UMDF_VERSION_MAJOR == 2 && UMDF_VERSION_MINOR == 0
  WPP_INIT_TRACING(MYDRIVER_TRACING_ID);
#else
  WPP_INIT_TRACING(DriverObject, RegistryPath);
#endif

  if (!g_pipe.Init())
  {
    status = STATUS_UNSUCCESSFUL;
    DEBUG_ERROR("Failed to setup IPC pipe");
    goto fail;
  }

  WDF_DRIVER_CONFIG config;
  WDF_OBJECT_ATTRIBUTES attributes;

  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

  WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
  attributes.EvtCleanupCallback = LGIddEvtDriverContextCleanup;
  WDF_DRIVER_CONFIG_INIT(&config, LGIddEvtDeviceAdd);

  CPlatformInfo::Init();
  status = WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config, WDF_NO_HANDLE);
  if (!NT_SUCCESS(status))
  {
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "WdfDriverCreate failed %!STATUS!", status);
    goto fail;
  }

  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
  return status;

fail:
#if UMDF_VERSION_MAJOR == 2 && UMDF_VERSION_MINOR == 0
  WPP_CLEANUP();
#else
  WPP_CLEANUP(DriverObject);
#endif
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

  g_pipe.DeInit();

#if UMDF_VERSION_MAJOR == 2 && UMDF_VERSION_MINOR == 0
  WPP_CLEANUP();
#else
  WPP_CLEANUP(WdfDriverWdmGetDriverObject((WDFDRIVER)DriverObject));
#endif
}