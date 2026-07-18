/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
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
#include "CPipeServer.h"
#include "CSettings.h"

WDFDEVICE l_wdfDevice = nullptr;

static const UINT IDDCX_VERSION_1_10 = 0x1A00;

static bool LGIddCanUseIddCx110DDIs(UINT iddCxVersion)
{
#ifdef HAS_IDDCX_110
  return iddCxVersion >= IDDCX_VERSION_1_10 &&
    !!IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2) &&
    !!IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorQueryHardwareCursor3) &&
    !!IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorUpdateModes2) &&
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo) &&
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterCommitModes2) &&
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxParseMonitorDescription2) &&
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorQueryTargetModes2) &&
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorSetDefaultHdrMetaData) &&
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorSetGammaRamp);
#else
  UNREFERENCED_PARAMETER(iddCxVersion);
  return false;
#endif
}

NTSTATUS LGIddDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previousState)
{
  UNREFERENCED_PARAMETER(previousState);

  DEBUG_INFO("Device entered D0, starting adapter initialization");
  auto * wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(device);
  wrapper->context->InitAdapter();

  DEBUG_INFO("Device D0 entry completed");
  return STATUS_SUCCESS;
}

NTSTATUS LGIddAdapterInitFinished(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED * args)
{
  DEBUG_INFO("Adapter initialization callback completed with status 0x%08x",
    args->AdapterInitStatus);

  auto * wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(adapter);
  if (!NT_SUCCESS(args->AdapterInitStatus))
  {
    DEBUG_ERROR_HR(args->AdapterInitStatus,
      "IddCx adapter initialization failed asynchronously");
    return STATUS_SUCCESS;
  }

  if (!wrapper->context)
  {
    DEBUG_ERROR("Adapter initialization completed before its context was attached");
    return STATUS_INVALID_DEVICE_STATE;
  }

  DEBUG_INFO("Adapter initialized, creating monitor");
  wrapper->context->FinishInit(0);
  return STATUS_SUCCESS;
}

NTSTATUS LGIddAdapterCommitModes(IDDCX_ADAPTER adapter, const IDARG_IN_COMMITMODES* args)
{
  UNREFERENCED_PARAMETER(adapter);
  UNREFERENCED_PARAMETER(args);
  DEBUG_INFO("Display modes committed");
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


#ifdef HAS_IDDCX_110
NTSTATUS LGIddParseMonitorDescription2(const IDARG_IN_PARSEMONITORDESCRIPTION2* inArgs,
  IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs)
{
  if (!l_wdfDevice)
    return STATUS_INVALID_PARAMETER;

  auto* wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(l_wdfDevice);
  return wrapper->context->ParseMonitorDescription2(inArgs, outArgs);
}

NTSTATUS LGIddAdapterQueryTargetInfo(IDDCX_ADAPTER adapter,
  IDARG_IN_QUERYTARGET_INFO* inArgs, IDARG_OUT_QUERYTARGET_INFO* outArgs)
{
  UNREFERENCED_PARAMETER(adapter);
  UNREFERENCED_PARAMETER(inArgs);

  outArgs->TargetCaps =
    (IDDCX_TARGET_CAPS)(IDDCX_TARGET_CAPS_WIDE_COLOR_SPACE | IDDCX_TARGET_CAPS_HIGH_COLOR_SPACE);
  outArgs->DitheringSupport.Rgb =
    (IDDCX_BITS_PER_COMPONENT)(IDDCX_BITS_PER_COMPONENT_8 | IDDCX_BITS_PER_COMPONENT_10);
  outArgs->DitheringSupport.YCbCr444 = IDDCX_BITS_PER_COMPONENT_NONE;
  outArgs->DitheringSupport.YCbCr422 = IDDCX_BITS_PER_COMPONENT_NONE;
  outArgs->DitheringSupport.YCbCr420 = IDDCX_BITS_PER_COMPONENT_NONE;
  return STATUS_SUCCESS;
}

NTSTATUS LGIddAdapterCommitModes2(IDDCX_ADAPTER adapter, const IDARG_IN_COMMITMODES2* args)
{
  UNREFERENCED_PARAMETER(adapter);
  UNREFERENCED_PARAMETER(args);
  DEBUG_INFO("Display modes committed through IddCx 1.10");
  return STATUS_SUCCESS;
}

NTSTATUS LGIddMonitorSetDefaultHdrMetadata(IDDCX_MONITOR monitor,
  const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA* inArgs)
{
  auto* wrapper = WdfObjectGet_CIndirectMonitorContextWrapper(monitor);
  auto* ctx     = wrapper->context->GetDeviceContext();

  ctx->SetHDRActive(inArgs->Data.pHdr10);

  return STATUS_SUCCESS;
}

NTSTATUS LGIddMonitorSetGammaRamp(IDDCX_MONITOR monitor, const IDARG_IN_SET_GAMMARAMP* inArgs)
{
  UNREFERENCED_PARAMETER(monitor);
  UNREFERENCED_PARAMETER(inArgs);
  return STATUS_SUCCESS;
}

NTSTATUS LGIddMonitorQueryTargetModes2(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES2* inArgs,
  IDARG_OUT_QUERYTARGETMODES* outArgs)
{
  auto* wrapper = WdfObjectGet_CIndirectMonitorContextWrapper(monitor);
  return wrapper->context->GetDeviceContext()->MonitorQueryTargetModes2(inArgs, outArgs);
}
#endif

NTSTATUS LGIddMonitorAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* inArgs)
{
  DEBUG_INFO("Swap chain assigned to monitor %p", monitor);
  auto * wrapper = WdfObjectGet_CIndirectMonitorContextWrapper(monitor);
  wrapper->context->AssignSwapChain(
    inArgs->hSwapChain, inArgs->RenderAdapterLuid, inArgs->hNextSurfaceAvailable);
  return STATUS_SUCCESS;
}

NTSTATUS LGIddMonitorUnassignSwapChain(IDDCX_MONITOR monitor)
{
  DEBUG_INFO("Swap chain unassigned from monitor %p", monitor);
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
    DEBUG_ERROR("IddCxGetVersion Failed");
    return status;
  }
  const bool hasIddCx110DDIs = LGIddCanUseIddCx110DDIs(ver.IddCxVersion);
  DEBUG_INFO("Version: 0x%04x", ver.IddCxVersion);
  DEBUG_INFO("IddCx 1.10 HDR/WCG DDIs: %s", hasIddCx110DDIs ? "available" : "unavailable");

  WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
  pnpPowerCallbacks.EvtDeviceD0Entry = LGIddDeviceD0Entry;
  WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnpPowerCallbacks);

  IDD_CX_CLIENT_CONFIG config;
  IDD_CX_CLIENT_CONFIG_INIT(&config);
  config.EvtIddCxAdapterInitFinished               = LGIddAdapterInitFinished;
  config.EvtIddCxMonitorGetDefaultDescriptionModes = LGIddMonitorGetDefaultModes;
  config.EvtIddCxMonitorAssignSwapChain            = LGIddMonitorAssignSwapChain;
  config.EvtIddCxMonitorUnassignSwapChain          = LGIddMonitorUnassignSwapChain;

#ifdef HAS_IDDCX_110
  if (hasIddCx110DDIs)
  {
    config.EvtIddCxParseMonitorDescription2      = LGIddParseMonitorDescription2;
    config.EvtIddCxMonitorQueryTargetModes2      = LGIddMonitorQueryTargetModes2;
    config.EvtIddCxAdapterCommitModes2           = LGIddAdapterCommitModes2;
    config.EvtIddCxAdapterQueryTargetInfo        = LGIddAdapterQueryTargetInfo;
    config.EvtIddCxMonitorSetDefaultHdrMetaData  = LGIddMonitorSetDefaultHdrMetadata;
    config.EvtIddCxMonitorSetGammaRamp           = LGIddMonitorSetGammaRamp;
  }
  else
#endif
  {
    config.EvtIddCxAdapterCommitModes      = LGIddAdapterCommitModes;
    config.EvtIddCxParseMonitorDescription = LGIddParseMonitorDescription;
    config.EvtIddCxMonitorQueryTargetModes = LGIddMonitorQueryTargetModes;
  }

  status = IddCxDeviceInitConfig(deviceInit, &config);
  if (!NT_SUCCESS(status))
    return status;

  WDF_OBJECT_ATTRIBUTES deviceAttributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, CIndirectDeviceContextWrapper);
  deviceAttributes.EvtCleanupCallback = [](WDFOBJECT object)
  {
    auto * wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(object);
    if (wrapper)
    {
      g_pipe.SetDeviceContext(nullptr);
      wrapper->Cleanup();
    }
    l_wdfDevice = nullptr;
  };

  WDFDEVICE device = nullptr;
  status = WdfDeviceCreate(&deviceInit, &deviceAttributes, &device);
  if (!NT_SUCCESS(status))
    return status;

  /*
   * Construct the device context and cache the WDF device BEFORE calling
   * IddCxDeviceInitialize. IddCxDeviceInitialize arms the IddCx callbacks, and
   * callbacks that resolve the context via l_wdfDevice (down-level IddCx that
   * provides no adapter/monitor context) must never observe a null context.
   */
  auto wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(device);
  wrapper->context = new CIndirectDeviceContext(device);

  l_wdfDevice = device;

  status = IddCxDeviceInitialize(device);
  if (!NT_SUCCESS(status))
    DEBUG_ERROR_HR(status, "IddCxDeviceInitialize Failed");
  else
    DEBUG_INFO("IddCx device initialized");
  return status;
}
