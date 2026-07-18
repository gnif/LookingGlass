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

#include "CIndirectDeviceContext.h"
#include "CIndirectMonitorContext.h"

#include "CSettings.h"
#include "CPlatformInfo.h"
#include "CPipeServer.h"
#include "CDebug.h"
#include "VersionInfo.h"

#include <dxgi1_2.h>
#include <sstream>

static const struct LGMPQueueConfig FRAME_QUEUE_CONFIG =
{
  LGMP_Q_FRAME,       //queueID
  LGMP_Q_FRAME_LEN,   //numMessages
  1000                //subTimeout
};

static const struct LGMPQueueConfig POINTER_QUEUE_CONFIG =
{
  LGMP_Q_POINTER,     //queueID
  LGMP_Q_POINTER_LEN, //numMesages
  1000                //subTimeout
};

static const UINT IDDCX_VERSION_1_10 = 0x1A00;

#ifdef HAS_IDDCX_110
static inline IDDCX_WIRE_BITS_PER_COMPONENT GetWireBitsPerComponent(bool hdr)
{
  IDDCX_WIRE_BITS_PER_COMPONENT bits = {};
  bits.Rgb = IDDCX_BITS_PER_COMPONENT_8;
  if (hdr)
    bits.Rgb = (IDDCX_BITS_PER_COMPONENT)(bits.Rgb |
      IDDCX_BITS_PER_COMPONENT_10 | IDDCX_BITS_PER_COMPONENT_16);
  bits.YCbCr444 = IDDCX_BITS_PER_COMPONENT_NONE;
  bits.YCbCr422 = IDDCX_BITS_PER_COMPONENT_NONE;
  bits.YCbCr420 = IDDCX_BITS_PER_COMPONENT_NONE;
  return bits;
}
#endif

void CIndirectDeviceContext::QueryIddCxCapabilities()
{
  IDARG_OUT_GETVERSION ver = {};
  NTSTATUS status = IddCxGetVersion(&ver);
  if (!NT_SUCCESS(status))
  {
    m_iddCxVersion = 0;
    m_hasIddCx110DDIs = false;
    m_canProcessFP16 = false;
    DEBUG_ERROR_HR(status, "IddCxGetVersion Failed");
    return;
  }

  m_iddCxVersion = ver.IddCxVersion;

#ifdef HAS_IDDCX_110
  const bool hasIddCx110DDIs =
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
  const bool hasIddCx110DDIs = false;
#endif

  m_hasIddCx110DDIs =
    m_iddCxVersion >= IDDCX_VERSION_1_10 && hasIddCx110DDIs;
  m_canProcessFP16 = !m_softwareMode && m_hasIddCx110DDIs;

  DEBUG_INFO("IddCx version: 0x%04x", m_iddCxVersion);
  DEBUG_INFO("IddCx 1.10 HDR/WCG DDIs: %s",
    m_hasIddCx110DDIs ? "available" : "unavailable");
  if (m_softwareMode && m_hasIddCx110DDIs)
    DEBUG_INFO("HDR/WCG disabled for software rendering");
}

void CIndirectDeviceContext::PopulateDefaultModes()
{
  g_settings.LoadModes();

  // Build the new mode list into a local first so we only hold the lock for
  // the swap. IddCx readers may be iterating the live container on another
  // thread; a clear()/push_back() under them would reallocate the backing
  // store and crash. std::move makes the publish a pointer swap.
  CSettings::DisplayModes newModes;
  newModes.reserve(g_settings.GetDisplayModes().size());
  for (auto& dm : g_settings.GetDisplayModes())
    newModes.push_back(dm);

  AcquireSRWLockExclusive(&m_modeLock);
  m_displayModes = std::move(newModes);
  ReleaseSRWLockExclusive(&m_modeLock);
}

void CIndirectDeviceContext::InitializeEdid()
{
  AcquireSRWLockExclusive(&m_modeLock);
  if (!m_edid.Size())
    m_edid.Build(m_displayModes, CanProcessFP16());
  ReleaseSRWLockExclusive(&m_modeLock);
}

void CIndirectDeviceContext::ScheduleInitRetry()
{
  // Create the retry timer once; if it already exists it is either running or
  // will be (re)started below.
  if (!m_initTimer)
  {
    WDF_TIMER_CONFIG config;
    WDF_TIMER_CONFIG_INIT_PERIODIC(&config,
      [](WDFTIMER timer) -> void
      {
        WDFOBJECT parent = WdfTimerGetParentObject(timer);
        auto wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(parent);
        wrapper->context->InitAdapter();
      },
      500);
    config.AutomaticSerialization = FALSE;

    WDF_OBJECT_ATTRIBUTES attribs;
    WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
    attribs.ParentObject   = m_wdfDevice;
    attribs.ExecutionLevel = WdfExecutionLevelDispatch;

    NTSTATUS status = WdfTimerCreate(&config, &attribs, &m_initTimer);
    if (!NT_SUCCESS(status))
    {
      DEBUG_ERROR_HR(status, "Init retry timer creation failed");
      m_initTimer = nullptr;
      return;
    }
  }

  WdfTimerStart(m_initTimer, WDF_REL_TIMEOUT_IN_MS(500));
}

void CIndirectDeviceContext::StopInitRetry()
{
  if (m_initTimer)
    WdfTimerStop(m_initTimer, FALSE);
}

void CIndirectDeviceContext::InitAdapter()
{
  DEBUG_TRACE("InitAdapter");

  // The adapter only needs to be created once. D0Entry and the retry timer can
  // both land here, so guard against re-entrancy and repeated creation.
  if (m_adapter)
  {
    DEBUG_TRACE("Adapter initialization skipped: adapter already exists");
    return;
  }

  if (InterlockedCompareExchange(&m_initInProgress, 1, 0) != 0)
  {
    DEBUG_TRACE("Adapter initialization skipped: initialization already in progress");
    return;
  }

  // At boot the IVSHMEM PCI device may not have enumerated yet. Rather than
  // silently abandoning the adapter (leaving the device loaded but with no
  // monitor), retry from a timer until the shared memory becomes available.
  if (!m_ivshmemOpened)
  {
    if (!m_ivshmem.Init() || !m_ivshmem.Open())
    {
      DEBUG_WARN("IVSHMEM not available yet, scheduling init retry");
      ScheduleInitRetry();
      InterlockedExchange(&m_initInProgress, 0);
      return;
    }
    m_ivshmemOpened = true;
  }

  // Select the render adapter before advertising capabilities. If no hardware
  // adapter is available, this is a software-rendered display and must remain
  // SDR-only; the software path must never depend on compute processing.
  bool havePreferredRenderAdapter = false;
  LUID preferredRenderAdapter = {};
  IDXGIFactory1 * factory = NULL;
  HRESULT factoryStatus = CreateDXGIFactory1(
    __uuidof(IDXGIFactory1), (void **)&factory);
  if (FAILED(factoryStatus))
    DEBUG_ERROR_HR(factoryStatus, "CreateDXGIFactory Failed");
  else
  {
    for (UINT i = 0;; ++i)
    {
      IDXGIAdapter1 * dxgiAdapter = nullptr;
      HRESULT enumStatus = factory->EnumAdapters1(i, &dxgiAdapter);
      if (enumStatus == DXGI_ERROR_NOT_FOUND)
        break;
      if (FAILED(enumStatus))
      {
        DEBUG_ERROR_HR(enumStatus, "Failed to enumerate DXGI adapter %u", i);
        break;
      }

      DXGI_ADAPTER_DESC1 adapterDesc = {};
      HRESULT descStatus = dxgiAdapter->GetDesc1(&adapterDesc);
      dxgiAdapter->Release();
      if (FAILED(descStatus))
      {
        DEBUG_ERROR_HR(descStatus, "Failed to query DXGI adapter %u", i);
        continue;
      }

      if ((adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
          (adapterDesc.VendorId == 0x1414 && adapterDesc.DeviceId == 0x008c))
      {
        DEBUG_INFO("Ignoring software render adapter %ls", adapterDesc.Description);
        continue;
      }

      if ((adapterDesc.VendorId == 0x1b36 && adapterDesc.DeviceId == 0x000d) || // QXL
          (adapterDesc.VendorId == 0x1234 && adapterDesc.DeviceId == 0x1111))   // QEMU Standard VGA
      {
        DEBUG_INFO("Ignoring display-only adapter %ls (vendor 0x%04x, device 0x%04x)",
          adapterDesc.Description, adapterDesc.VendorId, adapterDesc.DeviceId);
        continue;
      }

      DEBUG_INFO("Selected render adapter %ls (vendor 0x%04x, device 0x%04x)",
        adapterDesc.Description, adapterDesc.VendorId, adapterDesc.DeviceId);
      preferredRenderAdapter = adapterDesc.AdapterLuid;
      havePreferredRenderAdapter = true;
      break;
    }

    factory->Release();
  }

  m_softwareMode = !havePreferredRenderAdapter;
  if (m_softwareMode)
    DEBUG_INFO("No hardware render adapter available; using SDR software mode");

  QueryIddCxCapabilities();
  DEBUG_TRACE("Loading configured display modes");
  PopulateDefaultModes();
  DEBUG_TRACE("Initializing monitor EDID");
  InitializeEdid();

  AcquireSRWLockShared(&m_modeLock);
  const size_t modeCount = m_displayModes.size();
  const UINT edidSize = m_edid.Size();
  ReleaseSRWLockShared(&m_modeLock);
  DEBUG_INFO("Initializing adapter with %llu modes and a %u-byte EDID",
    (unsigned long long)modeCount, edidSize);

  IDDCX_ADAPTER_CAPS caps = {};
  caps.Size = sizeof(caps);

  /**
   * For some reason if we do not set this flag sometimes windows will
   * refuse to enumerate our virtual monitor. Intel also noted in their
   * sources that if this is not set dynamic resolution changes from this
   * driver will not work. This behaviour is not documented by Microsoft.
   */
  caps.Flags = IDDCX_ADAPTER_FLAGS_USE_SMALLEST_MODE;
#ifdef HAS_IDDCX_110
  if (CanProcessFP16())
    caps.Flags |= IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16;
#endif

  caps.MaxMonitorsSupported = 1;

  caps.EndPointDiagnostics.Size             = sizeof(caps.EndPointDiagnostics);
  caps.EndPointDiagnostics.GammaSupport     = IDDCX_FEATURE_IMPLEMENTATION_NONE;
  caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_OTHER;

  caps.EndPointDiagnostics.pEndPointFriendlyName     = L"Looking Glass IDD Driver";
  caps.EndPointDiagnostics.pEndPointManufacturerName = L"Looking Glass";
  caps.EndPointDiagnostics.pEndPointModelName        = L"Looking Glass";

  IDDCX_ENDPOINT_VERSION ver = {};
  ver.Size     = sizeof(ver);
  ver.MajorVer = 1;
  caps.EndPointDiagnostics.pFirmwareVersion = &ver;
  caps.EndPointDiagnostics.pHardwareVersion = &ver;

  WDF_OBJECT_ATTRIBUTES attr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, CIndirectDeviceContextWrapper);

  IDARG_IN_ADAPTER_INIT init = {};
  init.WdfDevice        = m_wdfDevice;
  init.pCaps            = &caps;
  init.ObjectAttributes = &attr;

  IDARG_OUT_ADAPTER_INIT initOut = {};
  DEBUG_INFO("Calling IddCxAdapterInitAsync with flags 0x%08x",
    caps.Flags);
  NTSTATUS status = IddCxAdapterInitAsync(&init, &initOut);
  if (!NT_SUCCESS(status) && CanProcessFP16())
  {
    DEBUG_WARN(
      "IddCxAdapterInitAsync rejected FP16 adapter capabilities (0x%08x), retrying without HDR/WCG",
      status);
    m_canProcessFP16 = false;
    // The monitor has not been created yet, so replace the provisional HDR
    // EDID before Windows can observe it.
    AcquireSRWLockExclusive(&m_modeLock);
    m_edid.Build(m_displayModes, false);
    ReleaseSRWLockExclusive(&m_modeLock);
    caps.Flags = (IDDCX_ADAPTER_FLAGS)(caps.Flags & ~IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16);
    ZeroMemory(&initOut, sizeof(initOut));
    status = IddCxAdapterInitAsync(&init, &initOut);
  }

  if (!NT_SUCCESS(status))
  {
    DEBUG_ERROR_HR(status, "IddCxAdapterInitAsync Failed");
    InterlockedExchange(&m_initInProgress, 0);
    return;
  }

  m_adapter = initOut.AdapterObject;
  if (!m_adapter)
  {
    DEBUG_ERROR("IddCxAdapterInitAsync succeeded without returning an adapter object");
    InterlockedExchange(&m_initInProgress, 0);
    return;
  }

  DEBUG_INFO("IddCxAdapterInitAsync started successfully (adapter %p)",
    m_adapter);

  // Try to co-exist with the virtual video device by telling IddCx which
  // hardware adapter we prefer to render on.
  if (havePreferredRenderAdapter)
  {
    IDARG_IN_ADAPTERSETRENDERADAPTER args = {};
    args.PreferredRenderAdapter = preferredRenderAdapter;
    IddCxAdapterSetRenderAdapter(m_adapter, &args);
    DEBUG_INFO("Preferred render adapter set");
  }

  auto * wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(m_adapter);
  wrapper->context = this;
  DEBUG_INFO("Adapter context attached; waiting for initialization callback");

  // Adapter is up; no need to keep retrying.
  StopInitRetry();
  InterlockedExchange(&m_initInProgress, 0);
  DEBUG_INFO("Adapter initialization request complete; returning to IddCx");
}

void CIndirectDeviceContext::FinishInit(UINT connectorIndex)
{
  DEBUG_INFO("Creating monitor on connector %u", connectorIndex);

  // We support a single monitor; never create a second one if one already
  // exists (a replug must clear m_monitor via departure first).
  AcquireSRWLockExclusive(&m_stateLock);
  bool haveMonitor = m_monitor != WDF_NO_HANDLE;
  ReleaseSRWLockExclusive(&m_stateLock);
  if (haveMonitor)
  {
    DEBUG_WARN("FinishInit skipped: a monitor already exists");
    return;
  }

  WDF_OBJECT_ATTRIBUTES attr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, CIndirectMonitorContextWrapper);

  // Take a private copy of the immutable EDID. The copy lives for the duration
  // of the synchronous create call below.
  std::vector<BYTE> edid;
  AcquireSRWLockShared(&m_modeLock);
  edid.assign(m_edid.Data(), m_edid.Data() + m_edid.Size());
  ReleaseSRWLockShared(&m_modeLock);
  DEBUG_INFO("Using %llu-byte monitor EDID", (unsigned long long)edid.size());

  IDDCX_MONITOR_INFO info = {};
  info.Size           = sizeof(info);
  info.MonitorType    = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
  info.ConnectorIndex = connectorIndex;

  info.MonitorDescription.Size     = sizeof(info.MonitorDescription);
  info.MonitorDescription.Type     = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
  info.MonitorDescription.DataSize = (UINT)edid.size();
  info.MonitorDescription.pData    = edid.empty() ? nullptr : edid.data();

  HRESULT hr = CoCreateGuid(&info.MonitorContainerId);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create the monitor container ID");
    return;
  }

  IDARG_IN_MONITORCREATE create = {};
  create.ObjectAttributes = &attr;
  create.pMonitorInfo     = &info;

  IDARG_OUT_MONITORCREATE createOut = {};
  NTSTATUS status = IddCxMonitorCreate(m_adapter, &create, &createOut);
  if (!NT_SUCCESS(status))
  {
    DEBUG_ERROR_HR(status, "IddCxMonitorCreate Failed");
    return;
  }

  DEBUG_INFO("Monitor object created (%p)", createOut.MonitorObject);

  AcquireSRWLockExclusive(&m_stateLock);
  m_monitor = createOut.MonitorObject;
  ReleaseSRWLockExclusive(&m_stateLock);

  auto * wrapper = WdfObjectGet_CIndirectMonitorContextWrapper(m_monitor);
  wrapper->context = new CIndirectMonitorContext(m_monitor, this);

  IDARG_OUT_MONITORARRIVAL out = {};
  status = IddCxMonitorArrival(m_monitor, &out);
  if (FAILED(status))
  {
    DEBUG_ERROR_HR(status, "IddCxMonitorArrival Failed");
    return;
  }

  DEBUG_INFO("Monitor arrival reported successfully");
}

void CIndirectDeviceContext::ReplugMonitor()
{
  AcquireSRWLockExclusive(&m_stateLock);

  if (m_replugMonitor || (m_swapChainAssigned && !m_swapChainReady))
  {
    // Coalesce changes received while a swap chain is being initialized, the
    // old one is draining, or its replacement is being initialized.
    m_replugPending = true;
    ReleaseSRWLockExclusive(&m_stateLock);
    return;
  }

  IDDCX_MONITOR monitor = m_monitor;
  if (monitor == WDF_NO_HANDLE)
  {
    m_replugMonitor   = true;
    m_monitorDeparted = true;
    ReleaseSRWLockExclusive(&m_stateLock);
    // Either no monitor yet, or one is already pending; build it now and
    // cancel any queued rebuild so we do not create two.
    InterlockedExchange(&m_finishInitQueued, 0);
    FinishInit(0);
    return;
  }

  // Clear the handle before departing so nothing calls an IddCx monitor API on
  // a departing/destroyed handle. FinishInit publishes the new one.
  m_replugMonitor             = true;
  m_monitorDeparted           = false;
  m_waitForSwapChainRelease   = m_swapChainAssigned;
  m_monitor                   = nullptr;
  ReleaseSRWLockExclusive(&m_stateLock);

  DEBUG_TRACE("ReplugMonitor");
  NTSTATUS status = IddCxMonitorDeparture(monitor);
  if (!NT_SUCCESS(status))
  {
    AcquireSRWLockExclusive(&m_stateLock);
    m_replugMonitor           = false;
    m_replugPending           = false;
    m_monitorDeparted         = false;
    m_waitForSwapChainRelease = false;
    m_monitor                 = monitor;
    ReleaseSRWLockExclusive(&m_stateLock);
    DEBUG_ERROR("IddCxMonitorDeparture Failed (0x%08x)", status);
    return;
  }

  AcquireSRWLockExclusive(&m_stateLock);
  m_monitorDeparted = true;
  const bool rebuild = !m_waitForSwapChainRelease;
  ReleaseSRWLockExclusive(&m_stateLock);

  // If there was no swap chain there will be no unassign callback to queue the
  // rebuild. Otherwise OnSwapChainReleased does so after teardown has drained.
  if (rebuild)
    InterlockedExchange(&m_finishInitQueued, 1);
}

void CIndirectDeviceContext::OnMonitorDestroyed(IDDCX_MONITOR monitor)
{
  AcquireSRWLockExclusive(&m_stateLock);
  if (m_monitor == monitor)
    m_monitor = nullptr;
  ReleaseSRWLockExclusive(&m_stateLock);
}

void CIndirectDeviceContext::OnSwapChainAssigned()
{
  AcquireSRWLockExclusive(&m_stateLock);
  m_swapChainAssigned = true;
  m_swapChainReady    = false;
  ReleaseSRWLockExclusive(&m_stateLock);
}

void CIndirectDeviceContext::OnSwapChainReleased()
{
  bool rebuild = false;

  AcquireSRWLockExclusive(&m_stateLock);
  m_swapChainAssigned = false;
  m_swapChainReady    = false;
  if (m_replugMonitor && m_waitForSwapChainRelease)
  {
    m_waitForSwapChainRelease = false;
    rebuild = m_monitorDeparted;
  }
  ReleaseSRWLockExclusive(&m_stateLock);

  if (rebuild)
    InterlockedExchange(&m_finishInitQueued, 1);
}

void CIndirectDeviceContext::OnSwapChainReady()
{
  bool replug    = false;
  bool doSetMode = false;
  CSettings::DisplayMode mode = {};

  AcquireSRWLockExclusive(&m_stateLock);
  m_swapChainReady = true;
  if (m_replugMonitor)
  {
    m_replugMonitor   = false;
    m_monitorDeparted = false;
    if (m_replugPending)
    {
      m_replugPending = false;
      replug = true;
    }
  }
  else if (m_replugPending)
  {
    m_replugPending = false;
    replug = true;
  }

  // Do not consume the requested mode on an intermediate replacement swap
  // chain. The last coalesced replug must be the one that applies it.
  if (!replug && m_doSetMode)
  {
    mode        = m_setMode;
    m_doSetMode = false;
    doSetMode   = true;
  }
  ReleaseSRWLockExclusive(&m_stateLock);

  // Do not expose the context to pipe reload requests until the initial swap
  // chain has reached the same ready state used by the replug gate.
  g_pipe.SetDeviceContext(this);

  if (replug)
    InterlockedExchange(&m_replugQueued, 1);
  else if (doSetMode)
    g_pipe.SetDisplayMode(mode.width, mode.height, mode.refresh);
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

NTSTATUS CIndirectDeviceContext::ParseMonitorDescription(
  const IDARG_IN_PARSEMONITORDESCRIPTION* inArgs,
  IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs)
{
  CSettings::DisplayModes modes;
  AcquireSRWLockShared(&m_modeLock);
  modes = m_displayModes;
  ReleaseSRWLockShared(&m_modeLock);

  outArgs->MonitorModeBufferOutputCount = (UINT)modes.size();
  outArgs->PreferredMonitorModeIdx = 0;
  if (inArgs->MonitorModeBufferInputCount < (UINT)modes.size())
    return (inArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  auto * mode = inArgs->pMonitorModes;
  for (auto it = modes.cbegin(); it != modes.cend(); ++it, ++mode)
  {
    mode->Size = sizeof(IDDCX_MONITOR_MODE);
    mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
    FillSignalInfo(mode->MonitorVideoSignalInfo, it->width, it->height, it->refresh, true);

    if (it->preferred)
      outArgs->PreferredMonitorModeIdx =
        (UINT)std::distance(modes.cbegin(), it);
  }

  return STATUS_SUCCESS;
}

NTSTATUS CIndirectDeviceContext::MonitorGetDefaultModes(
  const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* inArgs,
  IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* outArgs)
{
  CSettings::DisplayModes modes;
  AcquireSRWLockShared(&m_modeLock);
  modes = m_displayModes;
  ReleaseSRWLockShared(&m_modeLock);

  outArgs->DefaultMonitorModeBufferOutputCount = (UINT)modes.size();
  outArgs->PreferredMonitorModeIdx = 0;
  if (inArgs->DefaultMonitorModeBufferInputCount < (UINT)modes.size())
    return (inArgs->DefaultMonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  auto* mode = inArgs->pDefaultMonitorModes;
  for (auto it = modes.cbegin(); it != modes.cend(); ++it, ++mode)
  {
    mode->Size = sizeof(IDDCX_MONITOR_MODE);
    mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
    FillSignalInfo(mode->MonitorVideoSignalInfo, it->width, it->height, it->refresh, true);

    if (it->preferred)
      outArgs->PreferredMonitorModeIdx =
      (UINT)std::distance(modes.cbegin(), it);
  }

  return STATUS_SUCCESS;
}

NTSTATUS CIndirectDeviceContext::MonitorQueryTargetModes(
  const IDARG_IN_QUERYTARGETMODES* inArgs,
  IDARG_OUT_QUERYTARGETMODES* outArgs)
{
  CSettings::DisplayModes modes;
  AcquireSRWLockShared(&m_modeLock);
  modes = m_displayModes;
  ReleaseSRWLockShared(&m_modeLock);

  outArgs->TargetModeBufferOutputCount = (UINT)modes.size();
  if (inArgs->TargetModeBufferInputCount < (UINT)modes.size())
    return (inArgs->TargetModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  auto* mode = inArgs->pTargetModes;
  for (auto it = modes.cbegin(); it != modes.cend(); ++it, ++mode)
  {
    mode->Size = sizeof(IDDCX_TARGET_MODE);
    FillSignalInfo(mode->TargetVideoSignalInfo.targetVideoSignalInfo, it->width, it->height, it->refresh, false);
  }

  return STATUS_SUCCESS;
}


#ifdef HAS_IDDCX_110
NTSTATUS CIndirectDeviceContext::ParseMonitorDescription2(
  const IDARG_IN_PARSEMONITORDESCRIPTION2* inArgs,
  IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs)
{
  CSettings::DisplayModes modes;
  AcquireSRWLockShared(&m_modeLock);
  modes = m_displayModes;
  ReleaseSRWLockShared(&m_modeLock);

  outArgs->MonitorModeBufferOutputCount = (UINT)modes.size();
  outArgs->PreferredMonitorModeIdx = 0;
  if (inArgs->MonitorModeBufferInputCount < (UINT)modes.size())
    return (inArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  auto * mode = inArgs->pMonitorModes;
  for (auto it = modes.cbegin(); it != modes.cend(); ++it, ++mode)
  {
    ZeroMemory(mode, sizeof(*mode));
    mode->Size = sizeof(IDDCX_MONITOR_MODE2);
    mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
    FillSignalInfo(mode->MonitorVideoSignalInfo, it->width, it->height, it->refresh, true);
    mode->BitsPerComponent = GetWireBitsPerComponent(CanProcessFP16());

    if (it->preferred)
      outArgs->PreferredMonitorModeIdx =
        (UINT)std::distance(modes.cbegin(), it);
  }

  return STATUS_SUCCESS;
}

NTSTATUS CIndirectDeviceContext::MonitorQueryTargetModes2(
  const IDARG_IN_QUERYTARGETMODES2* inArgs,
  IDARG_OUT_QUERYTARGETMODES* outArgs)
{
  CSettings::DisplayModes modes;
  AcquireSRWLockShared(&m_modeLock);
  modes = m_displayModes;
  ReleaseSRWLockShared(&m_modeLock);

  outArgs->TargetModeBufferOutputCount = (UINT)modes.size();
  if (inArgs->TargetModeBufferInputCount < (UINT)modes.size())
    return STATUS_SUCCESS;

  if (!inArgs->pTargetModes)
    return STATUS_INVALID_PARAMETER;

  auto* mode = inArgs->pTargetModes;
  for (auto it = modes.cbegin(); it != modes.cend(); ++it, ++mode)
  {
    ZeroMemory(mode, sizeof(*mode));
    mode->Size = sizeof(IDDCX_TARGET_MODE2);
    FillSignalInfo(mode->TargetVideoSignalInfo.targetVideoSignalInfo, it->width, it->height, it->refresh, false);
    mode->BitsPerComponent = GetWireBitsPerComponent(CanProcessFP16());
  }

  return STATUS_SUCCESS;
}
#endif

void CIndirectDeviceContext::SetResolution(int width, int height)
{
  CSettings::DisplayMode mode = {};
  mode.width     = width;
  mode.height    = height;
  mode.refresh   = g_settings.GetDefaultRefresh();
  mode.preferred = true;

  AcquireSRWLockExclusive(&m_stateLock);
  m_setMode   = mode;
  m_doSetMode = true;
  ReleaseSRWLockExclusive(&m_stateLock);

  g_settings.SetExtraMode(mode);

  PopulateDefaultModes();

  // IddCxMonitorUpdateModes[2] does not invalidate Windows' cached mode list,
  // so the only reliable way to apply a new mode is to depart and re-arrive the
  // monitor, forcing Windows to rebuild the topology from the new mode list.
  ReplugMonitor();
}

bool CIndirectDeviceContext::SetupLGMP(size_t alignSize)
{
  // this may get called multiple times as we need to delay calling it until
  // we can determine the required alignment from the GPU in use
  if (m_lgmp)
    return true;

  m_alignSize = alignSize;

  std::stringstream ss;
  {
    KVMFR kvmfr = {};
    memcpy_s(kvmfr.magic, sizeof(kvmfr.magic), KVMFR_MAGIC, sizeof(KVMFR_MAGIC) - 1);
    kvmfr.version  = KVMFR_VERSION;
    kvmfr.features =
      KVMFR_FEATURE_SETCURSORPOS |
      KVMFR_FEATURE_WINDOWSIZE;
    strncpy_s(kvmfr.hostver, LG_VERSION_STR, sizeof(kvmfr.hostver) - 1);
    ss.write(reinterpret_cast<const char *>(&kvmfr), sizeof(kvmfr));
  }

  {
    const std::string & model = CPlatformInfo::GetCPUModel();

    KVMFRRecord_VMInfo * vmInfo = static_cast<KVMFRRecord_VMInfo *>(calloc(1, sizeof(*vmInfo)));
    if (!vmInfo)
    {
      DEBUG_ERROR("Failed to allocate KVMFRRecord_VMInfo");
      return false;
    }
    vmInfo->cpus    = static_cast<uint8_t>(CPlatformInfo::GetProcCount  ());
    vmInfo->cores   = static_cast<uint8_t>(CPlatformInfo::GetCoreCount  ());
    vmInfo->sockets = static_cast<uint8_t>(CPlatformInfo::GetSocketCount());

    const uint8_t * uuid = CPlatformInfo::GetUUID();
    memcpy_s (vmInfo->uuid, sizeof(vmInfo->uuid), uuid, 16);
    strncpy_s(vmInfo->capture, "Looking Glass IDD Driver", sizeof(vmInfo->capture));

    KVMFRRecord * record = static_cast<KVMFRRecord *>(calloc(1, sizeof(*record)));
    if (!record)
    {
      DEBUG_ERROR("Failed to allocate KVMFRRecord");
      return false;
    }

    record->type = KVMFR_RECORD_VMINFO;
    record->size = sizeof(*vmInfo) + (uint32_t)model.length() + 1;

    ss.write(reinterpret_cast<const char*>(record       ), sizeof(*record));
    ss.write(reinterpret_cast<const char*>(vmInfo       ), sizeof(*vmInfo));
    ss.write(reinterpret_cast<const char*>(model.c_str()), model.length() + 1);
  }

  {
    KVMFRRecord_OSInfo * osInfo = static_cast<KVMFRRecord_OSInfo *>(calloc(1, sizeof(*osInfo)));
    if (!osInfo)
    {
      DEBUG_ERROR("Failed to allocate KVMFRRecord_OSInfo");
      return false;
    }

    osInfo->os = KVMFR_OS_WINDOWS;

    const std::string & osName = CPlatformInfo::GetProductName();

    KVMFRRecord* record = static_cast<KVMFRRecord*>(calloc(1, sizeof(*record)));
    if (!record)
    {
      DEBUG_ERROR("Failed to allocate KVMFRRecord");
      return false;
    }

    record->type = KVMFR_RECORD_OSINFO;
    record->size = sizeof(*osInfo) + (uint32_t)osName.length() + 1;

    ss.write(reinterpret_cast<const char*>(record), sizeof(*record));
    ss.write(reinterpret_cast<const char*>(osInfo), sizeof(*osInfo));
    ss.write(reinterpret_cast<const char*>(osName.c_str()), osName.length() + 1);
  }

  LGMP_STATUS status;
  std::string udata = ss.str();

  if ((status = lgmpHostInit(m_ivshmem.GetMem(), (uint32_t)m_ivshmem.GetSize(),
    &m_lgmp, (uint32_t)udata.size(), (uint8_t*)&udata[0])) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostInit Failed: %s", lgmpStatusString(status));
    return false;
  }

  if ((status = lgmpHostQueueNew(m_lgmp, FRAME_QUEUE_CONFIG, &m_frameQueue)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueCreate Failed (Frame): %s", lgmpStatusString(status));
    return false;
  }

  if ((status = lgmpHostQueueNew(m_lgmp, POINTER_QUEUE_CONFIG, &m_pointerQueue)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueCreate Failed (Pointer): %s", lgmpStatusString(status));
    return false;
  }

  for (int i = 0; i < LGMP_Q_POINTER_LEN; ++i)
  {
    if ((status = lgmpHostMemAlloc(m_lgmp, MAX_POINTER_SIZE, &m_pointerMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Pointer): %s", lgmpStatusString(status));
      return false;
    }
    memset(lgmpHostMemPtr(m_pointerMemory[i]), 0, MAX_POINTER_SIZE);
  }

  for (int i = 0; i < POINTER_SHAPE_BUFFERS; ++i)
  {
    if ((status = lgmpHostMemAlloc(m_lgmp, MAX_POINTER_SIZE, &m_pointerShapeMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Pointer Shapes): %s", lgmpStatusString(status));
      return false;
    }
    memset(lgmpHostMemPtr(m_pointerShapeMemory[i]), 0, MAX_POINTER_SIZE);
  }

  for (int i = 0; i < COLOR_TRANSFORM_BUFFERS; ++i)
  {
    if ((status = lgmpHostMemAlloc(m_lgmp,
        sizeof(KVMFRCursor) + sizeof(KVMFRColorTransform),
        &m_pointerTransformMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Pointer Transform): %s",
        lgmpStatusString(status));
      return false;
    }
    memset(lgmpHostMemPtr(m_pointerTransformMemory[i]), 0,
      sizeof(KVMFRCursor) + sizeof(KVMFRColorTransform));
  }

  m_maxFrameSize = lgmpHostMemAvail(m_lgmp);
  m_maxFrameSize = (m_maxFrameSize -(m_alignSize - 1)) & ~(m_alignSize - 1);
  m_maxFrameSize /= LGMP_Q_FRAME_LEN;
  DEBUG_INFO("Max Frame Size: %u MiB", (unsigned int)(m_maxFrameSize / 1048576LL));

  for (int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    if ((status = lgmpHostMemAllocAligned(m_lgmp, (uint32_t)m_maxFrameSize,
        (uint32_t)m_alignSize, &m_frameMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAllocAligned Failed (Frame): %s", lgmpStatusString(status));
      return false;
    }

    m_frame[i] = (KVMFRFrame *)lgmpHostMemPtr(m_frameMemory[i]);

    /**
     * put the framebuffer on the border of the next page, this is to allow for
     * aligned DMA tranfers by the reciever */
    const size_t alignOffset = alignSize - sizeof(FrameBuffer);
    m_frame[i]->offset = (uint32_t)alignOffset;
    m_frameBuffer[i] = (FrameBuffer*)(((uint8_t*)m_frame[i]) + alignOffset);
  }

  WDF_TIMER_CONFIG config;
  WDF_TIMER_CONFIG_INIT_PERIODIC(&config,
    [](WDFTIMER timer) -> void
    {
      WDFOBJECT parent = WdfTimerGetParentObject(timer);
      auto wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(parent);
      wrapper->context->LGMPTimer();
    },
    10);
  config.AutomaticSerialization = FALSE;

  /**
  * documentation states that Dispatch is not available under the UDMF, however...
  * using Passive returns a not supported error, and Dispatch works.
  */
  WDF_OBJECT_ATTRIBUTES attribs;
  WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
  attribs.ParentObject   = m_wdfDevice;
  attribs.ExecutionLevel = WdfExecutionLevelDispatch;

  NTSTATUS s = WdfTimerCreate(&config, &attribs, &m_lgmpTimer);
  if (!NT_SUCCESS(s))
  {
    DEBUG_ERROR_HR(s, "Timer creation failed");
    return false;
  }
  WdfTimerStart(m_lgmpTimer, WDF_REL_TIMEOUT_IN_MS(10));

  return true;
}

void CIndirectDeviceContext::DeInitLGMP()
{
  InterlockedExchange(&m_publishedFrameIndex, -1);

  // The retry timer callback dereferences this context, so make sure it is
  // stopped and drained before we tear anything down. Wait for any in-flight
  // callback to complete.
  if (m_initTimer)
  {
    WdfTimerStop(m_initTimer, TRUE);
    m_initTimer = nullptr;
  }

  if (m_lgmp == nullptr)
    return;

  if (m_lgmpTimer)
  {
    WdfTimerStop(m_lgmpTimer, TRUE);
    m_lgmpTimer = nullptr;
  }

  for (int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    lgmpHostMemFree(&m_frameMemory[i]);
  for (int i = 0; i < LGMP_Q_POINTER_LEN; ++i)
    lgmpHostMemFree(&m_pointerMemory[i]);
  for (int i = 0; i < POINTER_SHAPE_BUFFERS; ++i)
    lgmpHostMemFree(&m_pointerShapeMemory[i]);

  for (int i = 0; i < COLOR_TRANSFORM_BUFFERS; ++i)
    lgmpHostMemFree(&m_pointerTransformMemory[i]);
  lgmpHostFree(&m_lgmp);
}

void CIndirectDeviceContext::LGMPTimer()
{
  // Rebuild the monitor queued by ReplugMonitor, off the IddCx callback thread.
  if (InterlockedExchange(&m_finishInitQueued, 0))
  {
    FinishInit(0);
    return;
  }

  if (InterlockedExchange(&m_replugQueued, 0))
  {
    ReplugMonitor();
    return;
  }

  LGMP_STATUS status;
  if ((status = lgmpHostProcess(m_lgmp)) != LGMP_OK)
  {
    if (status == LGMP_ERR_CORRUPTED)
    {
      DEBUG_WARN("LGMP reported the shared memory has been corrupted, attempting to recover\n");
      //TODO: fixme - reinit
      return;
    }

    DEBUG_ERROR("lgmpHostProcess Failed: %s", lgmpStatusString(status));
    //TODO: fixme - shutdown
    return;
  }

  uint8_t data[LGMP_MSGS_SIZE];
  size_t  size;
  while ((status = lgmpHostReadData(m_pointerQueue, &data, &size)) == LGMP_OK)
  {
    KVMFRMessage * msg = (KVMFRMessage *)data;
    switch (msg->type)
    {
      case KVMFR_MESSAGE_SETCURSORPOS:
      {
        KVMFRSetCursorPos* sp = (KVMFRSetCursorPos*)msg;
        g_pipe.SetCursorPos(sp->x, sp->y);
        break;
      }

      case KVMFR_MESSAGE_WINDOWSIZE:
      {
        KVMFRWindowSize* ws = (KVMFRWindowSize*)msg;
        SetResolution(ws->w, ws->h);
      }
    }

    lgmpHostAckData(m_pointerQueue);
  }

  if (lgmpHostQueueNewSubs(m_frameQueue) && m_monitor)
  {
    const LONG frameIndex =
      InterlockedCompareExchange(&m_publishedFrameIndex, 0, 0);
    if (frameIndex >= 0)
      lgmpHostQueuePost(m_frameQueue, 0, m_frameMemory[frameIndex]);
  }

  if (lgmpHostQueueNewSubs(m_pointerQueue))
  {
    ResendCursor();
    SendColorTransform();
  }
}

bool CIndirectDeviceContext::FrameBufferAvailable() const
{
  return m_lgmp && m_frameQueue &&
    lgmpHostQueuePending(m_frameQueue) < LGMP_Q_FRAME_LEN;
}

CIndirectDeviceContext::PreparedFrameBuffer CIndirectDeviceContext::PrepareFrameBuffer(
  unsigned pitch, const D12FrameFormat& srcFormat, const D12FrameFormat& dstFormat,
  const RECT * dirtyRects, unsigned nbDirtyRects)
{
  PreparedFrameBuffer result = {};

  if (!FrameBufferAvailable())
    return result;

  if (m_width     != dstFormat.desc.Width  ||
      m_height    != dstFormat.desc.Height ||
      m_pitch     != pitch                 ||
      m_format    != dstFormat.desc.Format ||
      m_frameType != dstFormat.format)
  {
    m_width     = (unsigned)dstFormat.desc.Width;
    m_height    = dstFormat.desc.Height;
    m_format    = dstFormat.desc.Format;
    m_frameType = dstFormat.format;
    m_pitch     = pitch;
    ++m_formatVer;
  }

  // Detect HDR metadata changes that require a format version bump
  // so the client knows to re-apply the HDR image description.
  //
  // Use dstFormat so post-processing can propagate any metadata adjustments.
  if (dstFormat.hdr)
  {
    const bool metadataChanged =
      m_lastHDRMetadata != dstFormat.hdrMetadata ||
      (dstFormat.hdrMetadata &&
       (memcmp(m_lastHDRDisplayPrimary, dstFormat.displayPrimary, sizeof(m_lastHDRDisplayPrimary)) != 0 ||
        memcmp(m_lastHDRWhitePoint    , dstFormat.whitePoint    , sizeof(m_lastHDRWhitePoint    )) != 0 ||
        m_lastHDRMaxDisplayLuminance       != dstFormat.maxDisplayLuminance       ||
        m_lastHDRMinDisplayLuminance       != dstFormat.minDisplayLuminance       ||
        m_lastHDRMaxContentLightLevel      != dstFormat.maxContentLightLevel      ||
        m_lastHDRMaxFrameAverageLightLevel != dstFormat.maxFrameAverageLightLevel));

    if (!m_lastHDRActive || metadataChanged || m_lastSDRWhiteLevel != dstFormat.sdrWhiteLevel)
      ++m_formatVer;
  }
  else if (m_lastHDRActive)
  {
    // HDR was turned off
    ++m_formatVer;
  }

  m_lastHDRActive   = dstFormat.hdr;
  m_lastHDRMetadata = dstFormat.hdrMetadata;
  memcpy(m_lastHDRDisplayPrimary, dstFormat.displayPrimary, sizeof(m_lastHDRDisplayPrimary));
  memcpy(m_lastHDRWhitePoint    , dstFormat.whitePoint    , sizeof(m_lastHDRWhitePoint    ));
  m_lastHDRMaxDisplayLuminance       = dstFormat.maxDisplayLuminance;
  m_lastHDRMinDisplayLuminance       = dstFormat.minDisplayLuminance;
  m_lastHDRMaxContentLightLevel      = dstFormat.maxContentLightLevel;
  m_lastHDRMaxFrameAverageLightLevel = dstFormat.maxFrameAverageLightLevel;
  m_lastSDRWhiteLevel                = dstFormat.sdrWhiteLevel;

  if (++m_frameIndex == LGMP_Q_FRAME_LEN)
    m_frameIndex = 0;

  KVMFRFrame * fi = m_frame[m_frameIndex];

  if (dstFormat.format == FRAME_TYPE_INVALID)
  {
    DEBUG_ERROR("Unsupported frame format, skipping frame");
    return result;
  }

  const unsigned maxRows = (unsigned)(m_maxFrameSize / pitch);
  const int bpp = dstFormat.format == FRAME_TYPE_RGBA16F ? 8 : 4;
  KVMFRFrameFlags flags =
    (dstFormat.hdr         ? FRAME_FLAG_HDR          : 0) |
    (dstFormat.hdrPQ       ? FRAME_FLAG_HDR_PQ       : 0) |
    (dstFormat.hdrMetadata ? FRAME_FLAG_HDR_METADATA : 0);

  if (maxRows < dstFormat.desc.Height)
    flags |= FRAME_FLAG_TRUNCATED;

  fi->formatVer        = m_formatVer;
  fi->frameSerial      = m_frameSerial++;
  fi->screenWidth      = srcFormat.width;
  fi->screenHeight     = srcFormat.height;
  fi->dataWidth        = (unsigned)dstFormat.desc.Width;
  fi->dataHeight       = min(maxRows, dstFormat.desc.Height);
  fi->frameWidth       = dstFormat.width;
  fi->frameHeight      = dstFormat.height;
  fi->stride           = pitch / bpp;
  fi->pitch            = pitch;
  // fi->offset is initialized at startup
  fi->flags            = flags;
  fi->sdrWhiteLevel    = dstFormat.sdrWhiteLevel;
  fi->rotation         = FRAME_ROT_0;
  fi->type             = dstFormat.format;

  if (flags & FRAME_FLAG_HDR_METADATA)
  {
    memcpy(fi->hdrDisplayPrimary, dstFormat.displayPrimary, sizeof(fi->hdrDisplayPrimary));
    memcpy(fi->hdrWhitePoint    , dstFormat.whitePoint    , sizeof(fi->hdrWhitePoint));
    fi->hdrMaxDisplayLuminance       = dstFormat.maxDisplayLuminance;
    fi->hdrMinDisplayLuminance       = dstFormat.minDisplayLuminance;
    fi->hdrMaxContentLightLevel      = dstFormat.maxContentLightLevel;
    fi->hdrMaxFrameAverageLightLevel = dstFormat.maxFrameAverageLightLevel;
  }
  else
  {
    memset(fi->hdrDisplayPrimary, 0, sizeof(fi->hdrDisplayPrimary));
    memset(fi->hdrWhitePoint    , 0, sizeof(fi->hdrWhitePoint    ));
    fi->hdrMaxDisplayLuminance       = 0;
    fi->hdrMinDisplayLuminance       = 0;
    fi->hdrMaxContentLightLevel      = 0;
    fi->hdrMaxFrameAverageLightLevel = 0;
  }

  fi->damageRectsCount = 0;
  if (nbDirtyRects <= ARRAYSIZE(fi->damageRects))
  {
    fi->damageRectsCount = nbDirtyRects;
    for (unsigned i = 0; i < nbDirtyRects; ++i)
    {
      fi->damageRects[i].x      = dirtyRects[i].left;
      fi->damageRects[i].y      = dirtyRects[i].top;
      fi->damageRects[i].width  = dirtyRects[i].right  - dirtyRects[i].left;
      fi->damageRects[i].height = dirtyRects[i].bottom - dirtyRects[i].top;
    }
  }

  FrameBuffer* fb = m_frameBuffer[m_frameIndex];
  fb->wp = 0;

  result.frameIndex = m_frameIndex;
  result.mem        = fb->data;

  return result;
}

bool CIndirectDeviceContext::PublishFrameBuffer(unsigned frameIndex)
{
  if (!m_frameQueue || frameIndex >= LGMP_Q_FRAME_LEN)
    return false;

  /* Make resends select this submitted frame before posting it. This prevents
   * a new subscriber racing publication from receiving the previous frame
   * after the new one. */
  InterlockedExchange(&m_publishedFrameIndex, (LONG)frameIndex);

  const LGMP_STATUS status =
    lgmpHostQueuePost(m_frameQueue, 0, m_frameMemory[frameIndex]);
  if (status != LGMP_OK)
  {
    DEBUG_ERROR("Failed to publish frame: %s", lgmpStatusString(status));
    return false;
  }

  return true;
}

void CIndirectDeviceContext::WriteFrameBuffer(unsigned frameIndex, void* src, size_t offset, size_t len, bool setWritePos) const
{
  FrameBuffer * fb = m_frameBuffer[frameIndex];

  memcpy(
    (void *)((uintptr_t)fb->data + offset),
    (void *)((uintptr_t)src + offset),
    len);

  if (setWritePos)
    fb->wp = (uint32_t)(offset + len);
}

void CIndirectDeviceContext::FinalizeFrameBuffer(unsigned frameIndex) const
{
  FrameBuffer * fb = m_frameBuffer[frameIndex];
  fb->wp = m_height * m_pitch;
}

void CIndirectDeviceContext::SendCursor(const IDARG_OUT_QUERY_HWCURSOR& info,
  const BYTE * data, UINT sdrWhiteLevel)
{
  PLGMPMemory mem;
  if (info.CursorShapeInfo.CursorType == IDDCX_CURSOR_SHAPE_TYPE_UNINITIALIZED)
  {
    mem = m_pointerMemory[m_pointerMemoryIndex];
    if (++m_pointerMemoryIndex == LGMP_Q_POINTER_LEN)
      m_pointerMemoryIndex = 0;
  }
  else
  {
    mem = m_pointerShapeMemory[m_pointerShapeIndex];
    if (++m_pointerShapeIndex == POINTER_SHAPE_BUFFERS)
      m_pointerShapeIndex = 0;
  }

  KVMFRCursor * cursor = (KVMFRCursor *)lgmpHostMemPtr(mem);
  cursor->sdrWhiteLevel = sdrWhiteLevel ?
    sdrWhiteLevel : KVMFR_SDR_WHITE_LEVEL_DEFAULT;

  m_cursorVisible = info.IsCursorVisible;
  uint32_t flags  = CURSOR_FLAG_VISIBLE_VALID;

  if (info.IsCursorVisible)
  {
    m_cursorX       = info.X;
    m_cursorY       = info.Y;
    cursor->x = (int16_t)info.X;
    cursor->y = (int16_t)info.Y;
    flags |= CURSOR_FLAG_POSITION | CURSOR_FLAG_VISIBLE;
  }

  if (info.CursorShapeInfo.CursorType != IDDCX_CURSOR_SHAPE_TYPE_UNINITIALIZED)
  {
    memcpy(cursor + 1, data,
      (size_t)info.CursorShapeInfo.Height * info.CursorShapeInfo.Pitch);

    cursor->hx     = (int8_t  )info.CursorShapeInfo.XHot;
    cursor->hy     = (int8_t  )info.CursorShapeInfo.YHot;
    cursor->width  = (uint32_t)info.CursorShapeInfo.Width;
    cursor->height = (uint32_t)info.CursorShapeInfo.Height;
    cursor->pitch  = (uint32_t)info.CursorShapeInfo.Pitch;

    switch (info.CursorShapeInfo.CursorType)
    {
      case IDDCX_CURSOR_SHAPE_TYPE_ALPHA:
        cursor->type = CURSOR_TYPE_COLOR;
        break;

      case IDDCX_CURSOR_SHAPE_TYPE_MASKED_COLOR:
        cursor->type = CURSOR_TYPE_MASKED_COLOR;
        break;
    }

    flags |= CURSOR_FLAG_SHAPE;
    m_pointerShape = mem;
  }

  LGMP_STATUS status;
  while ((status = lgmpHostQueuePost(m_pointerQueue, flags, mem)) != LGMP_OK)
  {
    if (status == LGMP_ERR_QUEUE_FULL)
    {
      Sleep(1);
      continue;
    }

    DEBUG_ERROR("lgmpHostQueuePost Failed (Pointer): %s", lgmpStatusString(status));
    break;
  }
}

void CIndirectDeviceContext::SetColorTransform(
  std::shared_ptr<const D12ColorTransform> transform)
{
  AcquireSRWLockExclusive(&m_colorTransformLock);
  m_colorTransform = std::move(transform);
  ReleaseSRWLockExclusive(&m_colorTransformLock);
  SendColorTransform();
}

std::shared_ptr<const D12ColorTransform>
CIndirectDeviceContext::GetColorTransform() const
{
  AcquireSRWLockShared(&m_colorTransformLock);
  auto transform = m_colorTransform;
  ReleaseSRWLockShared(&m_colorTransformLock);
  return transform;
}

#ifdef HAS_IDDCX_110
void CIndirectDeviceContext::SetHDRActive(const struct IDDCX_HDR10_METADATA * hdrMeta)
{
  AcquireSRWLockExclusive(&m_hdrLock);

  if (!hdrMeta)
  {
    m_hdrActive = false;
    ReleaseSRWLockExclusive(&m_hdrLock);
    return;
  }

  m_hdrActive = true;

  m_hdrDisplayPrimary[0][0] = hdrMeta->RedPrimary  [0];
  m_hdrDisplayPrimary[0][1] = hdrMeta->RedPrimary  [1];
  m_hdrDisplayPrimary[1][0] = hdrMeta->GreenPrimary[0];
  m_hdrDisplayPrimary[1][1] = hdrMeta->GreenPrimary[1];
  m_hdrDisplayPrimary[2][0] = hdrMeta->BluePrimary [0];
  m_hdrDisplayPrimary[2][1] = hdrMeta->BluePrimary [1];
  m_hdrWhitePoint    [0]    = hdrMeta->WhitePoint  [0];
  m_hdrWhitePoint    [1]    = hdrMeta->WhitePoint  [1];

  m_hdrMaxDisplayLuminance       = hdrMeta->MaxMasteringLuminance;
  m_hdrMinDisplayLuminance       = hdrMeta->MinMasteringLuminance;
  m_hdrMaxContentLightLevel      = hdrMeta->MaxContentLightLevel;
  m_hdrMaxFrameAverageLightLevel = hdrMeta->MaxFrameAverageLightLevel;

  ReleaseSRWLockExclusive(&m_hdrLock);
}
#endif

bool CIndirectDeviceContext::GetHDRMetadata(D12FrameFormat & format) const
{
  AcquireSRWLockShared(&m_hdrLock);

  if (!m_hdrActive)
  {
    ReleaseSRWLockShared(&m_hdrLock);
    return false;
  }

  memcpy(format.displayPrimary, m_hdrDisplayPrimary, sizeof(format.displayPrimary));
  memcpy(format.whitePoint    , m_hdrWhitePoint    , sizeof(format.whitePoint    ));
  format.maxDisplayLuminance       = m_hdrMaxDisplayLuminance;
  format.minDisplayLuminance       = m_hdrMinDisplayLuminance;
  format.maxContentLightLevel      = m_hdrMaxContentLightLevel;
  format.maxFrameAverageLightLevel = m_hdrMaxFrameAverageLightLevel;

  ReleaseSRWLockShared(&m_hdrLock);
  return true;
}

void CIndirectDeviceContext::SendColorTransform()
{
  if (!m_pointerQueue || !m_pointerTransformMemory[0])
    return;

  PLGMPMemory mem = m_pointerTransformMemory[m_pointerTransformIndex];
  if (++m_pointerTransformIndex == COLOR_TRANSFORM_BUFFERS)
    m_pointerTransformIndex = 0;

  KVMFRCursor * cursor = (KVMFRCursor *)lgmpHostMemPtr(mem);
  KVMFRColorTransform * output =
    (KVMFRColorTransform *)(cursor + 1);
  const auto transform = GetColorTransform();

  output->flags = 0;
  if (transform)
  {
    if (transform->matrixEnabled)
      output->flags |= KVMFR_COLOR_TRANSFORM_MATRIX;
    if (transform->lutEnabled)
      output->flags |= KVMFR_COLOR_TRANSFORM_LUT;
    memcpy(output->matrix, transform->matrix, sizeof(output->matrix));
    output->scalar = transform->scalar;
    memcpy(output->lut, transform->lut, sizeof(output->lut));
  }

  LGMP_STATUS status;
  while ((status = lgmpHostQueuePost(m_pointerQueue,
      CURSOR_FLAG_COLOR_TRANSFORM, mem)) != LGMP_OK)
  {
    if (status == LGMP_ERR_QUEUE_FULL)
    {
      Sleep(1);
      continue;
    }

    DEBUG_ERROR("lgmpHostQueuePost Failed (Pointer Transform): %s",
      lgmpStatusString(status));
    break;
  }
}

void CIndirectDeviceContext::ResendCursor()
{
  PLGMPMemory mem = m_pointerShape;
  if (!mem)
    return;

  KVMFRCursor* cursor = (KVMFRCursor*)lgmpHostMemPtr(mem);
  cursor->x = (int16_t)m_cursorX;
  cursor->y = (int16_t)m_cursorY;

  const uint32_t flags =
    CURSOR_FLAG_POSITION | CURSOR_FLAG_SHAPE | CURSOR_FLAG_VISIBLE_VALID |
    (m_cursorVisible ? CURSOR_FLAG_VISIBLE : 0);

  LGMP_STATUS status;
  while ((status = lgmpHostQueuePost(m_pointerQueue, flags, mem)) != LGMP_OK)
  {
    if (status == LGMP_ERR_QUEUE_FULL)
    {
      Sleep(1);
      continue;
    }

    DEBUG_ERROR("lgmpHostQueuePost Failed (Pointer): %s", lgmpStatusString(status));
    break;
  }
}
