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
#include "device.tmh"

#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <IddCx.h>
#include <avrt.h>
#include <wrl.h>

#include "CDebug.h"
#include "CIndirectDeviceContext.h"
#include "CIndirectMonitorContext.h"

WDFDEVICE l_wdfDevice = nullptr;

NTSTATUS LGIddDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previousState)
{
  UNREFERENCED_PARAMETER(previousState);
  
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
  if (!l_wdfDevice)
    return STATUS_INVALID_PARAMETER;

  auto* wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(l_wdfDevice);
  return wrapper->context->ParseMonitorDescription(inArgs, outArgs);
}

NTSTATUS LGIddMonitorGetDefaultModes(IDDCX_MONITOR monitor, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES * inArgs,
  IDARG_OUT_GETDEFAULTDESCRIPTIONMODES * outArgs)
{
  auto* wrapper = WdfObjectGet_CIndirectMonitorContextWrapper(monitor);
  return wrapper->context->GetDeviceContext()->MonitorGetDefaultModes(inArgs, outArgs);
}

NTSTATUS LGIddMonitorQueryTargetModes(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES * inArgs,
  IDARG_OUT_QUERYTARGETMODES * outArgs)
{
  auto* wrapper = WdfObjectGet_CIndirectMonitorContextWrapper(monitor);
  return wrapper->context->GetDeviceContext()->MonitorQueryTargetModes(inArgs, outArgs);
}

NTSTATUS LGIddMonitorAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* inArgs)
{
  auto * wrapper = WdfObjectGet_CIndirectMonitorContextWrapper(monitor);
  wrapper->context->AssignSwapChain(
    inArgs->hSwapChain, inArgs->RenderAdapterLuid, inArgs->hNextSurfaceAvailable);
  wrapper->context->GetDeviceContext()->OnAssignSwapChain();
  return STATUS_SUCCESS;
}

NTSTATUS LGIddMonitorUnassignSwapChain(IDDCX_MONITOR monitor)
{
  auto* wrapper = WdfObjectGet_CIndirectMonitorContextWrapper(monitor);
  wrapper->context->UnassignSwapChain();
  wrapper->context->GetDeviceContext()->OnUnassignedSwapChain();
  return STATUS_SUCCESS;
}

NTSTATUS LGIddCreateDevice(_Inout_ PWDFDEVICE_INIT deviceInit)
{
  NTSTATUS status;
  IDARG_OUT_GETVERSION ver;
  status = IddCxGetVersion(&ver);
  if (FAILED(status))
  {
    DEBUG_ERROR("IddCxGetVersion Failed");
    return status;
  }
  DEBUG_INFO("Version: 0x%04x", ver.IddCxVersion);

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
    l_wdfDevice = nullptr;
  };

  WDFDEVICE device = nullptr;
  status = WdfDeviceCreate(&deviceInit, &deviceAttributes, &device);
  if (!NT_SUCCESS(status))
    return status;

  /*
   * Because we are limited to IddCx 1.5 to retain Windows 10 support we have
   * no way of getting the device context in `LGIdddapterCommitModes`, as such
   * we need to store this for later.
   */
  l_wdfDevice = device;

  status = IddCxDeviceInitialize(device);

  auto wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(device);
  wrapper->context = new CIndirectDeviceContext(device);
  return status;
}
