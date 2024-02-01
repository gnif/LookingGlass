/**
 * Looking Glass
 * Copyright © 2017-2024 The Looking Glass Authors
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
#include "device.tmh"

#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <IddCx.h>
#include <avrt.h>
#include <wrl.h>

#include "Debug.h"
#include "CIndirectDeviceContext.h"
#include "CIndirectMonitorContext.h"

NTSTATUS LGIddDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previousState)
{
  UNREFERENCED_PARAMETER(previousState);
  UNREFERENCED_PARAMETER(device);
  
  auto * wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(device);
  wrapper->context->InitAdapter();

  return STATUS_SUCCESS;
}

NTSTATUS LGIddAdapterInitFinished(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED * args)
{
  auto * wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(adapter);
  if (!NT_SUCCESS(args->AdapterInitStatus))
    return STATUS_SUCCESS;

  wrapper->context->FinishInit(0);
  return STATUS_SUCCESS;
}

NTSTATUS LGIddAdapterCommitModes(IDDCX_ADAPTER adapter, const IDARG_IN_COMMITMODES* args)
{
  UNREFERENCED_PARAMETER(adapter);
  UNREFERENCED_PARAMETER(args);
  return STATUS_SUCCESS;
}

static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO & mode, DWORD width, DWORD height, DWORD vsync, bool monitorMode)
{
  mode.totalSize.cx = mode.activeSize.cx = width;
  mode.totalSize.cy = mode.activeSize.cy = height;

  mode.AdditionalSignalInfo.vSyncFreqDivider = monitorMode ? 0 : 1;
  mode.AdditionalSignalInfo.videoStandard    = 255;
  
  mode.vSyncFreq.Numerator   = vsync;
  mode.vSyncFreq.Denominator = 1;
  mode.hSyncFreq.Numerator   = vsync * height;
  mode.hSyncFreq.Denominator = 1;

  mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  mode.pixelRate        = ((UINT64)vsync) * ((UINT64)width) * ((UINT64)height);
}

NTSTATUS LGIddParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* inArgs,
  IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs)
{
  outArgs->MonitorModeBufferOutputCount = ARRAYSIZE(DisplayModes);
  if (inArgs->MonitorModeBufferInputCount < ARRAYSIZE(DisplayModes))
    return (inArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
   
  for (UINT i = 0; i < ARRAYSIZE(DisplayModes); ++i)
  {
    inArgs->pMonitorModes[i].Size   = sizeof(IDDCX_MONITOR_MODE);
    inArgs->pMonitorModes[i].Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
    FillSignalInfo(inArgs->pMonitorModes[i].MonitorVideoSignalInfo,
      DisplayModes[i][0], DisplayModes[i][1], DisplayModes[i][2], true);
  }

  outArgs->PreferredMonitorModeIdx = PreferredDisplayMode;
  return STATUS_SUCCESS;
}

NTSTATUS LGIddMonitorGetDefaultModes(IDDCX_MONITOR monitor, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES * inArgs,
  IDARG_OUT_GETDEFAULTDESCRIPTIONMODES * outArgs)
{
  UNREFERENCED_PARAMETER(monitor);

  outArgs->DefaultMonitorModeBufferOutputCount = ARRAYSIZE(DisplayModes);
  if (inArgs->DefaultMonitorModeBufferInputCount < ARRAYSIZE(DisplayModes))
    return (inArgs->DefaultMonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  for (UINT i = 0; i < ARRAYSIZE(DisplayModes); ++i)
  {
    inArgs->pDefaultMonitorModes[i].Size   = sizeof(IDDCX_MONITOR_MODE);
    inArgs->pDefaultMonitorModes[i].Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
    FillSignalInfo(inArgs->pDefaultMonitorModes[i].MonitorVideoSignalInfo,
      DisplayModes[i][0], DisplayModes[i][1], DisplayModes[i][2], true);
  }

  outArgs->PreferredMonitorModeIdx = PreferredDisplayMode;
  return STATUS_SUCCESS;
}

NTSTATUS LGIddMonitorQueryTargetModes(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES * inArgs,
  IDARG_OUT_QUERYTARGETMODES * outArgs)
{
  UNREFERENCED_PARAMETER(monitor);

  outArgs->TargetModeBufferOutputCount = ARRAYSIZE(DisplayModes);
  if (inArgs->TargetModeBufferInputCount < ARRAYSIZE(DisplayModes))
    return (inArgs->TargetModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  for (UINT i = 0; i < ARRAYSIZE(DisplayModes); ++i)
  {
    inArgs->pTargetModes[i].Size = sizeof(IDDCX_TARGET_MODE);    
    FillSignalInfo(inArgs->pTargetModes[i].TargetVideoSignalInfo.targetVideoSignalInfo,
      DisplayModes[i][0], DisplayModes[i][1], DisplayModes[i][2], false);
  }

  return STATUS_SUCCESS;
}

NTSTATUS LGIddMonitorAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* inArgs)
{
  auto * wrapper = WdfObjectGet_CIndirectMonitorContextWrapper(monitor);
  wrapper->context->AssignSwapChain(inArgs->hSwapChain, inArgs->RenderAdapterLuid, inArgs->hNextSurfaceAvailable);
  return STATUS_SUCCESS;
}

NTSTATUS LGIddMonitorUnassignSwapChain(IDDCX_MONITOR monitor)
{
  auto* wrapper = WdfObjectGet_CIndirectMonitorContextWrapper(monitor);
  wrapper->context->UnassignSwapChain();
  return STATUS_SUCCESS;
}

NTSTATUS LGIddCreateDevice(_Inout_ PWDFDEVICE_INIT deviceInit)
{
  NTSTATUS status;
  IDARG_OUT_GETVERSION ver;
  status = IddCxGetVersion(&ver);
  if (FAILED(status))
  {
    DBGPRINT("IddCxGetVersion Failed");
    return status;
  }
  DBGPRINT("Version: 0x%04x", ver.IddCxVersion);

  WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
  pnpPowerCallbacks.EvtDeviceD0Entry = LGIddDeviceD0Entry;
  WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnpPowerCallbacks);

  IDD_CX_CLIENT_CONFIG config;
  IDD_CX_CLIENT_CONFIG_INIT(&config);
  config.EvtIddCxAdapterInitFinished               = LGIddAdapterInitFinished;
  config.EvtIddCxAdapterCommitModes                = LGIddAdapterCommitModes;
  config.EvtIddCxParseMonitorDescription           = LGIddParseMonitorDescription;
  config.EvtIddCxMonitorGetDefaultDescriptionModes = LGIddMonitorGetDefaultModes;
  config.EvtIddCxMonitorQueryTargetModes           = LGIddMonitorQueryTargetModes;
  config.EvtIddCxMonitorAssignSwapChain            = LGIddMonitorAssignSwapChain;
  config.EvtIddCxMonitorUnassignSwapChain          = LGIddMonitorUnassignSwapChain;

  status = IddCxDeviceInitConfig(deviceInit, &config);
  if (!NT_SUCCESS(status))
    return status;

  WDF_OBJECT_ATTRIBUTES deviceAttributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, CIndirectDeviceContextWrapper);
  deviceAttributes.EvtCleanupCallback = [](WDFOBJECT object)
  {
    auto * wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(object);
    if (wrapper)
      wrapper->Cleanup();
  };

  WDFDEVICE device = nullptr;
  status = WdfDeviceCreate(&deviceInit, &deviceAttributes, &device);
  if (!NT_SUCCESS(status))
    return status;

  status = IddCxDeviceInitialize(device);

  auto wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(device);
  wrapper->context = new CIndirectDeviceContext(device);
  return status;
}
