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
#include <bcrypt.h>
#include <wrl.h>
#include <limits>
#include <memory>
#include <utility>

#include "CDebug.h"
#include "ClipboardRing.h"
#include "display/CDisplayConfiguration.h"
#include "display/IddCxCompat.h"
#include "display/CDeviceContext.h"
#include "display/CMonitorContext.h"
#include "transport/IControlTransport.h"
#include "ipc/CPipeServer.h"
#include "config/CSettings.h"

WDFDEVICE l_wdfDevice = nullptr;

static const UINT IDDCX_VERSION_1_10        = 0x1A00;
static uint64_t   l_authorityInstanceId[2]  = {};
static uint64_t   l_authorityProcessCreated = 0;

static uint64_t FileTimeValue(const FILETIME& value)
{
  ULARGE_INTEGER result = {};
  result.LowPart  = value.dwLowDateTime;
  result.HighPart = value.dwHighDateTime;
  return result.QuadPart;
}

static NTSTATUS InitAuthorityIdentity()
{
  FILETIME created = {};
  FILETIME exited  = {};
  FILETIME kernel  = {};
  FILETIME user    = {};
  if (!GetProcessTimes(GetCurrentProcess(),
      &created, &exited, &kernel, &user))
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to query the LGIdd host process creation time");
    return STATUS_UNSUCCESSFUL;
  }

  for (unsigned attempt = 0; attempt < 2; ++attempt)
  {
    const NTSTATUS status = BCryptGenRandom(nullptr,
      reinterpret_cast<PUCHAR>(l_authorityInstanceId),
      sizeof(l_authorityInstanceId), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!NT_SUCCESS(status))
    {
      DEBUG_ERROR_HR(HRESULT_FROM_NT(status),
        "Failed to generate the LGIdd authority instance identifier");
      return status;
    }
    if (l_authorityInstanceId[0] && l_authorityInstanceId[1])
    {
      l_authorityProcessCreated = FileTimeValue(created);
      return STATUS_SUCCESS;
    }
  }

  DEBUG_ERROR_HR(ERROR_INVALID_DATA,
    "Generated an invalid LGIdd authority instance identifier");
  return STATUS_DATA_ERROR;
}

static bool AuthorityInstanceMatches(const uint64_t (&instanceId)[2])
{
  return instanceId[0] == l_authorityInstanceId[0] &&
    instanceId[1] == l_authorityInstanceId[1];
}

static void LGIddAuthorityFileCleanup(WDFFILEOBJECT fileObject)
{
  g_pipe.CloseClipboardAuthorityFile(fileObject);
}

static void LGIddAuthorityIoDeviceControl(WDFDEVICE device,
  WDFREQUEST request, size_t outputLength, size_t inputLength,
  ULONG controlCode)
{
  UNREFERENCED_PARAMETER(device);

  NTSTATUS      status      = STATUS_INVALID_DEVICE_REQUEST;
  ULONG_PTR     information = 0;
  WDFFILEOBJECT fileObject  = WdfRequestGetFileObject(request);
  if (!fileObject)
  {
    DEBUG_WARN(
      "LGIdd authority request has no file object");
    WdfRequestComplete(request, STATUS_INVALID_HANDLE);
    return;
  }

  switch (controlCode)
  {
    case IOCTL_LG_IDD_AUTHORITY_GET_HOST:
    {
      if (inputLength || outputLength < sizeof(LGIddAuthorityHost))
      {
        DEBUG_WARN(
          "Rejected malformed LGIdd authority host request");
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }

      LGIddAuthorityHost * host = nullptr;
      status = WdfRequestRetrieveOutputBuffer(request, sizeof(*host),
        reinterpret_cast<void **>(&host), nullptr);
      if (!NT_SUCCESS(status))
      {
        DEBUG_WARN_HR(HRESULT_FROM_NT(status),
          "Failed to retrieve the LGIdd authority host output buffer");
        break;
      }

      ZeroMemory(host, sizeof(*host));
      host->size           = sizeof(*host);
      host->version        = LG_IDD_AUTHORITY_VERSION;
      host->processId      = GetCurrentProcessId();
      host->processCreated = l_authorityProcessCreated;
      host->instanceId[0]  = l_authorityInstanceId[0];
      host->instanceId[1]  = l_authorityInstanceId[1];
      information          = sizeof(*host);
      status               = STATUS_SUCCESS;
      break;
    }

    case IOCTL_LG_IDD_AUTHORITY_REGISTER:
    {
      if (inputLength != sizeof(LGIddAuthorityRegistration) || outputLength)
      {
        DEBUG_WARN(
          "Rejected malformed LGIdd authority registration request");
        status = STATUS_INFO_LENGTH_MISMATCH;
        break;
      }

      void * input = nullptr;
      status = WdfRequestRetrieveInputBuffer(request,
        sizeof(LGIddAuthorityRegistration), &input, nullptr);
      if (!NT_SUCCESS(status))
      {
        DEBUG_WARN_HR(HRESULT_FROM_NT(status),
          "Failed to retrieve the LGIdd authority registration buffer");
        break;
      }
      const LGIddAuthorityRegistration * registration =
        static_cast<const LGIddAuthorityRegistration *>(input);
      if (registration->size != sizeof(*registration) ||
          registration->version != LG_IDD_AUTHORITY_VERSION ||
          registration->reserved ||
          !AuthorityInstanceMatches(registration->instanceId) ||
          !registration->mappingId[0] || !registration->mappingId[1] ||
          !registration->mappingHandle ||
          registration->mappingHandle >
            static_cast<uint64_t>(
              (std::numeric_limits<uintptr_t>::max)()) ||
          registration->mappingHandle == static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(INVALID_HANDLE_VALUE)))
      {
        DEBUG_WARN(
          "Rejected invalid LGIdd authority registration data");
        status = STATUS_DATA_ERROR;
        break;
      }

      HANDLE rawMapping = reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(registration->mappingHandle));
      const auto closeInjectedMapping = [rawMapping]()
      {
        if (!CloseHandle(rawMapping))
        {
          const DWORD error = GetLastError();
          DEBUG_WARN_HR(error,
            "Failed to consume the injected clipboard authority handle");
          return false;
        }
        return true;
      };
      const ClipboardMapping * view = static_cast<const ClipboardMapping *>(
        MapViewOfFileFromApp(rawMapping,
          FILE_MAP_READ | FILE_MAP_WRITE, 0, sizeof(ClipboardMapping)));
      if (!view)
      {
        const DWORD error = GetLastError();
        DEBUG_WARN_HR(error,
          "Failed to validate the injected clipboard authority handle");
        closeInjectedMapping();
        status = STATUS_INVALID_HANDLE;
        break;
      }
      const uint64_t authorityId[2] =
        { view->authorityId[0], view->authorityId[1] };
      if (!UnmapViewOfFile(view))
      {
        const DWORD error = GetLastError();
        DEBUG_WARN_HR(error,
          "Failed to unmap the injected clipboard authority handle");
        closeInjectedMapping();
        status = STATUS_UNSUCCESSFUL;
        break;
      }
      if (!authorityId[0] || !authorityId[1])
      {
        DEBUG_WARN("Rejected clipboard mapping without an authority ID");
        closeInjectedMapping();
        status = STATUS_DATA_ERROR;
        break;
      }

      HANDLE mapping = nullptr;
      if (!DuplicateHandle(GetCurrentProcess(), rawMapping,
          GetCurrentProcess(), &mapping,
          SECTION_MAP_READ | SECTION_MAP_WRITE, FALSE, 0))
      {
        const DWORD error = GetLastError();
        DEBUG_WARN_HR(error,
          "Failed to privatize the injected clipboard authority handle");
        closeInjectedMapping();
        status = STATUS_INVALID_HANDLE;
        break;
      }
      if (!closeInjectedMapping())
      {
        CloseHandle(mapping);
        status = STATUS_UNSUCCESSFUL;
        break;
      }

      if (!g_pipe.RegisterClipboardAuthority(fileObject, mapping,
          registration->session, registration->mappingId, authorityId))
      {
        CloseHandle(mapping);
        status = STATUS_ACCESS_DENIED;
        break;
      }

      status = STATUS_SUCCESS;
      break;
    }

    case IOCTL_LG_IDD_AUTHORITY_CLEAR:
    {
      if (inputLength != sizeof(LGIddAuthorityClear) || outputLength)
      {
        DEBUG_WARN(
          "Rejected malformed LGIdd authority clear request");
        status = STATUS_INFO_LENGTH_MISMATCH;
        break;
      }

      void * input = nullptr;
      status = WdfRequestRetrieveInputBuffer(request,
        sizeof(LGIddAuthorityClear), &input, nullptr);
      if (!NT_SUCCESS(status))
      {
        DEBUG_WARN_HR(HRESULT_FROM_NT(status),
          "Failed to retrieve the LGIdd authority clear buffer");
        break;
      }
      const LGIddAuthorityClear * clear =
        static_cast<const LGIddAuthorityClear *>(input);
      if (clear->size != sizeof(*clear) ||
          clear->version != LG_IDD_AUTHORITY_VERSION ||
          !AuthorityInstanceMatches(clear->instanceId))
      {
        DEBUG_WARN(
          "Rejected invalid LGIdd authority clear data");
        status = STATUS_DATA_ERROR;
        break;
      }

      if (!g_pipe.ClearClipboardAuthority(fileObject))
      {
        status = STATUS_ACCESS_DENIED;
        break;
      }
      status = STATUS_SUCCESS;
      break;
    }
  }

  WdfRequestCompleteWithInformation(request, status, information);
}

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
  auto * wrapper = WdfObjectGet_CDeviceContextWrapper(device);
  wrapper->context->InitAdapter();

  DEBUG_INFO("Device D0 entry completed");
  return STATUS_SUCCESS;
}

NTSTATUS LGIddAdapterInitFinished(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED * args)
{
  DEBUG_INFO("Adapter initialization callback completed with status 0x%08x",
    args->AdapterInitStatus);

  auto * wrapper = WdfObjectGet_CDeviceContextWrapper(adapter);
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
  wrapper->context->FinishAdapterInit(0);
  return STATUS_SUCCESS;
}

NTSTATUS LGIddAdapterCommitModes(IDDCX_ADAPTER adapter, const IDARG_IN_COMMITMODES* args)
{
  UNREFERENCED_PARAMETER(adapter);
  UNREFERENCED_PARAMETER(args);
  DEBUG_INFO("Display modes committed");
  return STATUS_SUCCESS;
}

NTSTATUS LGIddParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* inArgs,
  IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs)
{
  if (!l_wdfDevice)
    return STATUS_INVALID_PARAMETER;

  auto * wrapper = WdfObjectGet_CDeviceContextWrapper(l_wdfDevice);
  return wrapper->context->GetDisplayConfiguration().ParseMonitorDescription(
    inArgs, outArgs);
}

NTSTATUS LGIddMonitorGetDefaultModes(IDDCX_MONITOR monitor, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES * inArgs,
  IDARG_OUT_GETDEFAULTDESCRIPTIONMODES * outArgs)
{
  auto * wrapper = WdfObjectGet_CMonitorContextWrapper(monitor);
  auto * context = wrapper->context->GetDeviceContext();
  return context->GetDisplayConfiguration().MonitorGetDefaultModes(
    inArgs, outArgs);
}

NTSTATUS LGIddMonitorQueryTargetModes(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES * inArgs,
  IDARG_OUT_QUERYTARGETMODES * outArgs)
{
  auto * wrapper = WdfObjectGet_CMonitorContextWrapper(monitor);
  auto * context = wrapper->context->GetDeviceContext();
  return context->GetDisplayConfiguration().MonitorQueryTargetModes(
    inArgs, outArgs);
}


#ifdef HAS_IDDCX_110
NTSTATUS LGIddParseMonitorDescription2(const IDARG_IN_PARSEMONITORDESCRIPTION2* inArgs,
  IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs)
{
  if (!l_wdfDevice)
    return STATUS_INVALID_PARAMETER;

  auto * wrapper = WdfObjectGet_CDeviceContextWrapper(l_wdfDevice);
  return wrapper->context->GetDisplayConfiguration().ParseMonitorDescription2(
    inArgs, outArgs);
}

NTSTATUS LGIddAdapterQueryTargetInfo(IDDCX_ADAPTER adapter,
  IDARG_IN_QUERYTARGET_INFO* inArgs, IDARG_OUT_QUERYTARGET_INFO* outArgs)
{
  UNREFERENCED_PARAMETER(inArgs);

  auto * wrapper = WdfObjectGet_CDeviceContextWrapper(adapter);
  const bool hdr = wrapper && wrapper->context &&
    wrapper->context->CanProcessFP16();

  outArgs->TargetCaps = hdr ?
    (IDDCX_TARGET_CAPS)(IDDCX_TARGET_CAPS_WIDE_COLOR_SPACE |
      IDDCX_TARGET_CAPS_HIGH_COLOR_SPACE) :
    (IDDCX_TARGET_CAPS)0;

  outArgs->DitheringSupport.Rgb      = IDDCX_BITS_PER_COMPONENT_NONE;
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
  UNREFERENCED_PARAMETER(monitor);
  UNREFERENCED_PARAMETER(inArgs);

  // This metadata describes the virtual monitor. Explicit per-content HDR10
  // metadata is obtained from the swap chain and forwarded separately.
  return STATUS_SUCCESS;
}

NTSTATUS LGIddMonitorSetGammaRamp(IDDCX_MONITOR monitor, const IDARG_IN_SET_GAMMARAMP* inArgs)
{
  auto * wrapper = WdfObjectGet_CMonitorContextWrapper(monitor);
  auto * ctx     = wrapper->context->GetDeviceContext();
  auto & control = ctx->GetTransport().Control();

  if (ctx->IsSoftwareMode())
  {
    control.SetColorTransform(nullptr);
    DEBUG_INFO("Ignoring display color transform in software mode");
    return STATUS_SUCCESS;
  }

  if (inArgs->Type == IDDCX_GAMMARAMP_TYPE_DEFAULT)
  {
    control.SetColorTransform(nullptr);
    DEBUG_INFO("Display color transform reset to default");
    return STATUS_SUCCESS;
  }

  if (inArgs->Type != IDDCX_GAMMARAMP_TYPE_3x4_COLORSPACE_TRANSFORM)
  {
    DEBUG_WARN("Unsupported gamma ramp type %u", inArgs->Type);
    return STATUS_NOT_SUPPORTED;
  }

  if (!inArgs->pGammaRampData ||
      inArgs->GammaRampSizeInBytes <
        sizeof(IDDCX_GAMMARAMP_3X4_COLORSPACE_TRANSFORM))
  {
    DEBUG_ERROR("Invalid 3x4 color transform buffer (%u bytes)",
      inArgs->GammaRampSizeInBytes);
    return STATUS_INVALID_PARAMETER;
  }

  const auto * input = static_cast<
    const IDDCX_GAMMARAMP_3X4_COLORSPACE_TRANSFORM*>(
      inArgs->pGammaRampData);
  auto transform = std::make_shared<D12ColorTransform>();
  transform->matrixEnabled = !!input->MatrixEnabled;
  transform->scalar        = input->ScalarMultiplier;
  transform->lutEnabled    = !!input->LutEnabled;
  memcpy(transform->matrix, input->ColorMatrix3x4,
    sizeof(transform->matrix));
  for (unsigned i = 0; i < 4096; ++i)
  {
    transform->lut[i][0] = input->LookupTable1D[i].Red;
    transform->lut[i][1] = input->LookupTable1D[i].Green;
    transform->lut[i][2] = input->LookupTable1D[i].Blue;
    transform->lut[i][3] = 1.0f;
  }

  auto active = D12::Transform(transform);
  if (!active)
  {
    control.SetColorTransform(nullptr);
    return STATUS_SUCCESS;
  }

  control.SetColorTransform(std::move(active));
  DEBUG_INFO("Display color transform updated (matrix:%d lut:%d)",
    input->MatrixEnabled, input->LutEnabled);
  return STATUS_SUCCESS;
}

NTSTATUS LGIddMonitorQueryTargetModes2(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES2* inArgs,
  IDARG_OUT_QUERYTARGETMODES* outArgs)
{
  auto * wrapper = WdfObjectGet_CMonitorContextWrapper(monitor);
  auto * context = wrapper->context->GetDeviceContext();
  return context->GetDisplayConfiguration().MonitorQueryTargetModes2(
    inArgs, outArgs);
}
#endif

NTSTATUS LGIddMonitorAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* inArgs)
{
  DEBUG_INFO("Swap chain assigned to monitor %p", monitor);
  auto * wrapper = WdfObjectGet_CMonitorContextWrapper(monitor);
  return wrapper->context->AssignSwapChain(
    inArgs->hSwapChain, inArgs->RenderAdapterLuid, inArgs->hNextSurfaceAvailable);
}

NTSTATUS LGIddMonitorUnassignSwapChain(IDDCX_MONITOR monitor)
{
  DEBUG_INFO("Swap chain unassigned from monitor %p", monitor);
  auto * wrapper = WdfObjectGet_CMonitorContextWrapper(monitor);
  wrapper->context->UnassignSwapChain();
  return STATUS_SUCCESS;
}

NTSTATUS LGIddCreateDevice(_Inout_ PWDFDEVICE_INIT deviceInit)
{
  NTSTATUS status;
  if (!l_authorityInstanceId[0] || !l_authorityInstanceId[1])
  {
    status = InitAuthorityIdentity();
    if (!NT_SUCCESS(status))
      return status;
  }

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
  config.EvtIddCxDeviceIoControl                   = LGIddAuthorityIoDeviceControl;
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

  WDF_FILEOBJECT_CONFIG fileConfig;
  WDF_FILEOBJECT_CONFIG_INIT(&fileConfig,
    WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK,
    LGIddAuthorityFileCleanup);
  WDF_OBJECT_ATTRIBUTES fileAttributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
    &fileAttributes, LGIddAuthorityFileContext);
  WdfDeviceInitSetFileObjectConfig(
    deviceInit, &fileConfig, &fileAttributes);

  WDF_OBJECT_ATTRIBUTES deviceAttributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, CDeviceContextWrapper);
  deviceAttributes.EvtCleanupCallback = [](WDFOBJECT object)
  {
    auto * wrapper = WdfObjectGet_CDeviceContextWrapper(object);
    if (wrapper)
    {
      g_pipe.ClearClipboardAuthority();
      g_pipe.SetDeviceContext(nullptr);
      wrapper->Cleanup();
    }
    l_wdfDevice = nullptr;
  };

  WDFDEVICE device = nullptr;
  status = WdfDeviceCreate(&deviceInit, &deviceAttributes, &device);
  if (!NT_SUCCESS(status))
    return status;

  status = WdfDeviceCreateDeviceInterface(
    device, &GUID_DEVINTERFACE_LGIdd, nullptr);
  if (!NT_SUCCESS(status))
  {
    DEBUG_ERROR_HR(HRESULT_FROM_NT(status),
      "Failed to create the LGIdd authority device interface");
    return status;
  }

  /*
   * Construct the device context and cache the WDF device BEFORE calling
   * IddCxDeviceInitialize. IddCxDeviceInitialize arms the IddCx callbacks, and
   * callbacks that resolve the context via l_wdfDevice (down-level IddCx that
   * provides no adapter/monitor context) must never observe a null context.
   */
  auto wrapper = WdfObjectGet_CDeviceContextWrapper(device);
  wrapper->context = new CDeviceContext(device);

  l_wdfDevice = device;

  status = IddCxDeviceInitialize(device);
  if (!NT_SUCCESS(status))
    DEBUG_ERROR_HR(status, "IddCxDeviceInitialize Failed");
  else
    DEBUG_INFO("IddCx device initialized");
  return status;
}
