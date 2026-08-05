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

static uint64_t FrameScheduleToken(
  const CFrameScheduler::Schedule& schedule)
{
  return static_cast<uint64_t>(schedule.generation) << 32 |
    schedule.epoch;
}

static bool FrameScheduleMatches(
  const CFrameScheduler::Schedule& a,
  const CFrameScheduler::Schedule& b)
{
  return a.clientID   == b.clientID   &&
         a.generation == b.generation &&
         a.epoch      == b.epoch;
}

static const UINT IDDCX_VERSION_1_10 = 0x1A00;

static const UINT64 FRAME_BYTES_PER_PIXEL = 4;

static bool AlignUp(UINT64 value, UINT64 alignment, UINT64& result)
{
  if (!alignment || (alignment & (alignment - 1)))
    return false;

  const UINT64 mask = alignment - 1;
  result = (value + mask) & ~mask;
  return true;
}

static bool CalculateFrameSize(uint32_t width, uint32_t height,
  UINT64& frameSize)
{
  frameSize = 0;
  if (!width || !height)
    return false;

  UINT64 pitch;
  if (!AlignUp((UINT64)width * FRAME_BYTES_PER_PIXEL,
      D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, pitch))
    return false;

  frameSize = pitch * height;
  return true;
}

static uint32_t RecommendedIVSHMEMSizeMiB(UINT64 requiredSize)
{
  UINT64 sizeMiB = requiredSize / 1048576;
  if (requiredSize % 1048576)
    ++sizeMiB;

  UINT64 result = 1;
  while (result < sizeMiB && result <= UINT32_MAX / 2)
    result <<= 1;

  return result < sizeMiB ? UINT32_MAX : (uint32_t)result;
}

#ifdef HAS_IDDCX_110
static inline IDDCX_WIRE_BITS_PER_COMPONENT GetWireBitsPerComponent(bool hdr)
{
  IDDCX_WIRE_BITS_PER_COMPONENT bits = {};
  // This describes the virtual monitor wire, not the swap-chain format.
  // HDR uses a 10-bpc PQ wire while CAN_PROCESS_FP16 requests the FP16/scRGB
  // source surface that Looking Glass converts for transport.
  bits.Rgb = IDDCX_BITS_PER_COMPONENT_8;
  if (hdr)
    bits.Rgb = (IDDCX_BITS_PER_COMPONENT)(bits.Rgb |
      IDDCX_BITS_PER_COMPONENT_10);
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

bool CIndirectDeviceContext::PopulateDefaultModes()
{
  const CSettings::DisplayModes configuredModes =
    g_settings.LoadModes();

  // Build the new mode list into a local first so we only hold the lock for
  // the swap. IddCx readers may be iterating the live container on another
  // thread; a clear()/push_back() under them would reallocate the backing
  // store and crash. std::move makes the publish a pointer swap.
  CSettings::DisplayModes newModes;
  newModes.reserve(configuredModes.size());

  const UINT64 alignment = m_alignSize ? m_alignSize :
    D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

  bool hasPreferred = false;
  for (const auto& configuredMode : configuredModes)
  {
    UINT64 frameSize;
    UINT64 requiredIVSHMEMSize;
    if (!GetResolutionMemoryRequirements(configuredMode.width,
        configuredMode.height, alignment, frameSize, requiredIVSHMEMSize))
    {
      DEBUG_WARN("Filtering invalid %s mode %ux%u@%u",
        configuredMode.extraMode ? "extra" : "configured",
        configuredMode.width, configuredMode.height,
        configuredMode.refresh);
      continue;
    }

    if (requiredIVSHMEMSize > m_ivshmem.GetSize())
    {
      DEBUG_WARN(
        "Filtering %s mode %ux%u@%u: requires %llu bytes of IVSHMEM, only %llu bytes are available",
        configuredMode.extraMode ? "extra" : "configured",
        configuredMode.width, configuredMode.height,
        configuredMode.refresh,
        (unsigned long long)requiredIVSHMEMSize,
        (unsigned long long)m_ivshmem.GetSize());
      continue;
    }

    CSettings::DisplayMode mode = configuredMode;
    if (mode.preferred)
    {
      mode.preferred = !hasPreferred;
      hasPreferred   = true;
    }
    newModes.push_back(mode);
  }

  if (newModes.empty())
  {
    DEBUG_ERROR("No configured display modes fit in IVSHMEM");
    return false;
  }

  // ExtraMode may have been the preferred mode. If it did not fit, promote
  // the first remaining mode so the list still has a valid preference.
  if (!hasPreferred)
    newModes.front().preferred = true;

  AcquireSRWLockExclusive(&m_modeLock);
  m_displayModes = std::move(newModes);
  ReleaseSRWLockExclusive(&m_modeLock);
  return true;
}

void CIndirectDeviceContext::InitializeEdid()
{
  AcquireSRWLockExclusive(&m_modeLock);
  if (!m_edid.Size())
    m_edid.Build(CanProcessFP16());
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

  LONG initExpected = 0;
  if (!m_initInProgress.compare_exchange_strong(initExpected, 1))
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
      m_initInProgress.store(0);
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
  DEBUG_TRACE("Initializing LGMP metadata");
  if (!InitializeLGMP())
  {
    m_initInProgress.store(0);
    return;
  }
  DEBUG_TRACE("Loading configured display modes");
  if (!PopulateDefaultModes())
  {
    m_initInProgress.store(0);
    return;
  }
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
    m_edid.Build(false);
    ReleaseSRWLockExclusive(&m_modeLock);
    caps.Flags = (IDDCX_ADAPTER_FLAGS)(caps.Flags & ~IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16);
    ZeroMemory(&initOut, sizeof(initOut));
    status = IddCxAdapterInitAsync(&init, &initOut);
  }

  if (!NT_SUCCESS(status))
  {
    DEBUG_ERROR_HR(status, "IddCxAdapterInitAsync Failed");
    m_initInProgress.store(0);
    return;
  }

  m_adapter = initOut.AdapterObject;
  if (!m_adapter)
  {
    DEBUG_ERROR("IddCxAdapterInitAsync succeeded without returning an adapter object");
    m_initInProgress.store(0);
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
  m_initInProgress.store(0);
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
    m_finishInitQueued.store(0);
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
    m_finishInitQueued.store(1);
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
    m_finishInitQueued.store(1);
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
    m_replugQueued.store(1);
  else if (doSetMode)
    g_pipe.SetDisplayMode(mode.width, mode.height, mode.refresh);
}

static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& signal,
  const CSettings::DisplayMode& mode, bool monitorMode)
{
  CEdid::Timing timing;
  if (!CEdid::GetTiming(timing, mode))
    return;

  signal.activeSize.cx = timing.hActive;
  signal.activeSize.cy = timing.vActive;
  signal.totalSize.cx  = timing.hActive + timing.hBlank;
  signal.totalSize.cy  = timing.vActive + timing.vBlank;

  signal.AdditionalSignalInfo.vSyncFreqDivider = monitorMode ? 0 : 1;
  signal.AdditionalSignalInfo.videoStandard    = 255;

  signal.vSyncFreq.Numerator   = mode.refresh;
  signal.vSyncFreq.Denominator = 1;
  signal.hSyncFreq.Numerator   = mode.refresh * signal.totalSize.cy;
  signal.hSyncFreq.Denominator = 1;

  signal.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  signal.pixelRate        = timing.pixelClock;
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
    mode->Size   = sizeof(IDDCX_MONITOR_MODE);
    mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
    FillSignalInfo(mode->MonitorVideoSignalInfo, *it, true);

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
    mode->Size   = sizeof(IDDCX_MONITOR_MODE);
    mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
    FillSignalInfo(mode->MonitorVideoSignalInfo, *it, true);

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
    FillSignalInfo(mode->TargetVideoSignalInfo.targetVideoSignalInfo, *it, false);
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
    mode->Size             = sizeof(IDDCX_MONITOR_MODE2);
    mode->Origin           = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
    FillSignalInfo(mode->MonitorVideoSignalInfo, *it, true);
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
    FillSignalInfo(mode->TargetVideoSignalInfo.targetVideoSignalInfo, *it, false);
    mode->BitsPerComponent = GetWireBitsPerComponent(CanProcessFP16());
  }

  return STATUS_SUCCESS;
}
#endif

bool CIndirectDeviceContext::GetResolutionMemoryRequirements(
  uint32_t width, uint32_t height, UINT64 alignment,
  UINT64& frameSize, UINT64& ivshmemSize) const
{
  frameSize   = 0;
  ivshmemSize = 0;

  if (!alignment || !m_frameMemoryOffset ||
      !CalculateFrameSize(width, height, frameSize))
    return false;

  UINT64 frameAllocationSize;
  if (!AlignUp(frameSize + alignment, alignment,
        frameAllocationSize))
    return false;

  UINT64 frameMemoryStart;
  if (!AlignUp(m_frameMemoryOffset, alignment, frameMemoryStart))
    return false;

  ivshmemSize = frameMemoryStart +
    frameAllocationSize * LGMP_Q_FRAME_BUFFER_LEN;
  return true;
}

void CIndirectDeviceContext::SetResolution(uint32_t width, uint32_t height)
{
  UINT64 frameSize;
  UINT64 requiredIVSHMEMSize;
  if (!GetResolutionMemoryRequirements(width, height, m_alignSize,
      frameSize, requiredIVSHMEMSize))
  {
    DEBUG_WARN("Ignoring invalid resolution request: %ux%u", width, height);
    return;
  }

  if (requiredIVSHMEMSize > m_ivshmem.GetSize())
  {
    const uint32_t requiredMiB =
      RecommendedIVSHMEMSizeMiB(requiredIVSHMEMSize);
    DEBUG_WARN(
      "Refusing resolution %ux%u: frame requires %llu bytes, only %llu bytes are available; IVSHMEM must be at least %u MiB",
      width, height,
      (unsigned long long)frameSize,
      (unsigned long long)m_maxFrameSize,
      requiredMiB);
    g_pipe.ResolutionRejected(width, height, requiredMiB);
    return;
  }

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

  if (!PopulateDefaultModes())
  {
    DEBUG_ERROR("Failed to rebuild the display mode list");
    return;
  }

  // IddCxMonitorUpdateModes[2] does not invalidate Windows' cached mode list,
  // so the only reliable way to apply a new mode is to depart and re-arrive the
  // monitor, forcing Windows to rebuild the topology from the new mode list.
  ReplugMonitor();
}

bool CIndirectDeviceContext::InitializeLGMP()
{
  if (m_lgmp)
    return true;

  std::stringstream ss;
  {
    KVMFR kvmfr = {};
    memcpy_s(kvmfr.magic, sizeof(kvmfr.magic), KVMFR_MAGIC, sizeof(KVMFR_MAGIC) - 1);
    kvmfr.version  = KVMFR_VERSION;
    kvmfr.features =
      KVMFR_FEATURE_SETCURSORPOS |
      KVMFR_FEATURE_WINDOWSIZE   |
      KVMFR_FEATURE_FRAME_SCHEDULE;
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

  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    const struct LGMPQueueConfig config =
    {
      LGMP_Q_FRAME_OWNER + i, //queueID
      LGMP_Q_FRAME_LEN,       //numMessages
      1000                    //subTimeout
    };
    if ((status = lgmpHostQueueNew(
        m_lgmp, config, &m_frameOwnerQueue[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostQueueCreate Failed (Frame Owner %u): %s",
        i, lgmpStatusString(status));
      return false;
    }
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

  m_frameMemoryOffset = m_ivshmem.GetSize() - lgmpHostMemAvail(m_lgmp);
  return true;
}

bool CIndirectDeviceContext::SetupLGMP(size_t alignSize)
{
  // This may get called multiple times as we need to delay allocating the
  // frame buffers until the GPU-specific alignment is known.
  if (m_maxFrameSize)
    return true;

  if (!InitializeLGMP())
    return false;

  m_alignSize = alignSize;

  if (!m_alignSize || (m_alignSize & (m_alignSize - 1)) ||
      m_alignSize < sizeof(KVMFRFrame) + sizeof(FrameBuffer))
  {
    DEBUG_ERROR("Invalid frame buffer alignment: %llu",
      (unsigned long long)m_alignSize);
    return false;
  }

  const size_t available = lgmpHostMemAvail(m_lgmp);

  UINT64 alignedFrameMemoryOffset;
  if (!AlignUp(m_frameMemoryOffset, m_alignSize,
      alignedFrameMemoryOffset))
  {
    DEBUG_ERROR("Unable to align the frame memory offset");
    return false;
  }

  const size_t alignmentPadding =
    (size_t)(alignedFrameMemoryOffset - m_frameMemoryOffset);
  if (available <= alignmentPadding)
  {
    DEBUG_ERROR("Insufficient shared memory for frame buffers");
    return false;
  }

  const size_t alignmentMask = m_alignSize - 1;
  size_t frameAllocationSize =
    (available - alignmentPadding) / LGMP_Q_FRAME_BUFFER_LEN;
  frameAllocationSize &= ~alignmentMask;
  if (frameAllocationSize <= m_alignSize ||
      frameAllocationSize > UINT32_MAX)
  {
    DEBUG_ERROR("Invalid frame allocation size: %llu",
      (unsigned long long)frameAllocationSize);
    return false;
  }

  // The KVMFR frame header and FrameBuffer write position occupy the first
  // alignment unit. Only the bytes after it are usable for pixel data.
  const size_t maxFrameSize = frameAllocationSize - m_alignSize;
  DEBUG_INFO("Max Frame Data Size: %u MiB",
    (unsigned int)(maxFrameSize / 1048576));

  LGMP_STATUS status;
  for (int i = 0; i < LGMP_Q_FRAME_BUFFER_LEN; ++i)
  {
    if ((status = lgmpHostMemAllocAligned(m_lgmp,
        (uint32_t)frameAllocationSize,
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
    m_frameInFlight[i].store(false, std::memory_order_release);
  }

  m_maxFrameSize = maxFrameSize;
  m_publishedFrameIndex.store(-1, std::memory_order_release);
  m_frameResendPending   = false;
  m_framePublishSequence = 0;
  memset(m_frameLastPublishSequence, 0,
    sizeof(m_frameLastPublishSequence));
  for (FrameDelivery& delivery : m_frameDelivery)
    delivery = {};
  for (OwnerDelivery& delivery : m_ownerDelivery)
    delivery = {};

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
  // The retry timer callback dereferences this context, so make sure it is
  // stopped and drained before we tear anything down. Wait for any in-flight
  // callback to complete.
  if (m_initTimer)
  {
    WdfTimerStop(m_initTimer, TRUE);
    m_initTimer = nullptr;
  }

  if (m_lgmp == nullptr)
  {
    m_frameScheduler.Reset();
    m_publishedFrameIndex.store(-1, std::memory_order_release);
    m_frameResendPending   = false;
    m_framePublishSequence = 0;
    memset(m_frameLastPublishSequence, 0,
      sizeof(m_frameLastPublishSequence));
    for (FrameDelivery& delivery : m_frameDelivery)
      delivery = {};
    for (OwnerDelivery& delivery : m_ownerDelivery)
      delivery = {};
    return;
  }

  if (m_lgmpTimer)
  {
    WdfTimerStop(m_lgmpTimer, TRUE);
    m_lgmpTimer = nullptr;
  }

  m_frameScheduler.Reset();

  AcquireSRWLockExclusive(&m_framePublishLock);
  m_publishedFrameIndex.store(-1, std::memory_order_release);
  m_frameResendPending   = false;
  m_framePublishSequence = 0;
  memset(m_frameLastPublishSequence, 0,
    sizeof(m_frameLastPublishSequence));
  for (FrameDelivery& delivery : m_frameDelivery)
    delivery = {};
  for (OwnerDelivery& delivery : m_ownerDelivery)
    delivery = {};
  ReleaseSRWLockExclusive(&m_framePublishLock);

  for (int i = 0; i < LGMP_Q_FRAME_BUFFER_LEN; ++i)
    m_frameInFlight[i].store(false, std::memory_order_release);

  for (int i = 0; i < LGMP_Q_FRAME_BUFFER_LEN; ++i)
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
  if (m_finishInitQueued.exchange(0))
  {
    FinishInit(0);
    return;
  }

  if (m_replugQueued.exchange(0))
  {
    ReplugMonitor();
    return;
  }

  LGMP_STATUS status;
  AcquireSRWLockExclusive(&m_lgmpProcessLock);
  status = lgmpHostProcess(m_lgmp);
  ReleaseSRWLockExclusive(&m_lgmpProcessLock);
  if (status != LGMP_OK)
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

  const uint64_t now = CFrameScheduler::Nanotime();
  uint32_t    clientIDs[LGMP_MAX_CLIENTS] = {};
  unsigned    clientCount                = 0;
  LGMP_STATUS subscriberStatus = lgmpHostGetClientIDs(
    m_frameQueue, clientIDs, &clientCount);
  for (unsigned queueIndex = 0;
       subscriberStatus == LGMP_OK && queueIndex < LGMP_Q_FRAME_LEN;
       ++queueIndex)
  {
    uint32_t queueClientIDs[LGMP_MAX_CLIENTS] = {};
    unsigned queueClientCount                = 0;
    subscriberStatus = lgmpHostGetClientIDs(
      m_frameOwnerQueue[queueIndex], queueClientIDs, &queueClientCount);

    unsigned commonCount = 0;
    for (unsigned i = 0;
         subscriberStatus == LGMP_OK && i < clientCount;
         ++i)
      for (unsigned candidate = 0; candidate < queueClientCount; ++candidate)
        if (clientIDs[i] == queueClientIDs[candidate])
        {
          clientIDs[commonCount++] = clientIDs[i];
          break;
        }
    clientCount = commonCount;
  }

  uint8_t data[LGMP_MSGS_SIZE];
  size_t  size;
  uint32_t sourceClientID;
  while ((status = lgmpHostReadDataWithSource(
      m_pointerQueue, &data, &size, &sourceClientID)) == LGMP_OK)
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
        break;
      }

      case KVMFR_MESSAGE_FRAME_SCHEDULE:
      {
        const KVMFRFrameSchedule * frameSchedule =
          reinterpret_cast<KVMFRFrameSchedule *>(msg);
        const bool valid = size == sizeof(*frameSchedule) &&
          m_frameScheduler.UpdateSchedule(
            sourceClientID, *frameSchedule, now);
        if (!valid)
          DEBUG_WARN("Ignoring invalid KVMFR frame schedule");
        else if ((frameSchedule->flags &
              (KVMFR_FRAME_SCHEDULE_ACTIVE |
               KVMFR_FRAME_SCHEDULE_IMMEDIATE)) ==
            (KVMFR_FRAME_SCHEDULE_ACTIVE |
             KVMFR_FRAME_SCHEDULE_IMMEDIATE))
        {
          CFrameScheduler::Schedule owner = {};
          if (!m_frameScheduler.GetSchedule(owner) ||
              owner.clientID != sourceClientID)
          {
            AcquireSRWLockExclusive(&m_framePublishLock);
            m_frameResendPending = true;
            ReleaseSRWLockExclusive(&m_framePublishLock);
          }
        }
        break;
      }
    }

    lgmpHostAckData(m_pointerQueue);
  }

  if (subscriberStatus == LGMP_OK)
    m_frameScheduler.UpdateSubscribers(clientIDs, clientCount, now);
  else
    DEBUG_WARN("Failed to query LGMP frame subscribers: %s",
      lgmpStatusString(subscriberStatus));

  m_frameScheduler.LogStatistics(now);

  if (lgmpHostQueueNewSubs(m_frameQueue))
  {
    AcquireSRWLockExclusive(&m_framePublishLock);
    m_frameResendPending = true;
    ReleaseSRWLockExclusive(&m_framePublishLock);
  }

  ProcessFrameDeliveries();

  if (lgmpHostQueueNewSubs(m_pointerQueue))
  {
    ResendCursor();
    SendColorTransform();
  }
}

bool CIndirectDeviceContext::PostSharedFrame(unsigned frameIndex,
  uint32_t excludeClientID)
{
  if (frameIndex >= LGMP_Q_FRAME_BUFFER_LEN ||
      lgmpHostQueuePending(m_frameQueue) != 0)
    return false;

  uint32_t clientIDs[LGMP_MAX_CLIENTS] = {};
  unsigned clientCount                = 0;
  LGMP_STATUS status =
    lgmpHostGetClientIDs(m_frameQueue, clientIDs, &clientCount);
  if (status != LGMP_OK)
  {
    DEBUG_ERROR("Failed to query shared frame subscribers: %s",
      lgmpStatusString(status));
    return false;
  }

  unsigned targetCount = 0;
  for (unsigned i = 0; i < clientCount; ++i)
  {
    bool excluded = clientIDs[i] == excludeClientID;
    for (const OwnerDelivery& delivery : m_ownerDelivery)
      if (delivery.active && delivery.clientID == clientIDs[i])
      {
        excluded = true;
        break;
      }

    if (!excluded)
      clientIDs[targetCount++] = clientIDs[i];
  }

  if (!targetCount)
  {
    m_frameDelivery[frameIndex].sharedOwnerToken    = 0;
    m_frameDelivery[frameIndex].sharedOwnerClientID = 0;
    m_frameDelivery[frameIndex].sharedOwnerPending  = false;
    m_frameDelivery[frameIndex].sharedPending       = false;
    m_frameDelivery[frameIndex].sharedDelivered     = true;
    m_frameResendPending                             = false;
    return true;
  }

  unsigned recipientCount = 0;
  status = lgmpHostQueuePostForClients(
    m_frameQueue, 0, m_frameMemory[frameIndex],
    clientIDs, targetCount, &recipientCount);
  if (status != LGMP_OK)
  {
    if (status != LGMP_ERR_QUEUE_FULL)
      DEBUG_ERROR("Failed to publish shared frame: %s",
        lgmpStatusString(status));
    return false;
  }

  if (!recipientCount)
  {
    m_frameDelivery[frameIndex].sharedOwnerToken    = 0;
    m_frameDelivery[frameIndex].sharedOwnerClientID = 0;
    m_frameDelivery[frameIndex].sharedOwnerPending  = false;
    m_frameDelivery[frameIndex].sharedPending       = false;
    m_frameDelivery[frameIndex].sharedDelivered     = true;
    m_frameResendPending                             = false;
    return true;
  }

  m_frameDelivery[frameIndex].sharedOwnerToken    = 0;
  m_frameDelivery[frameIndex].sharedOwnerClientID = 0;
  m_frameDelivery[frameIndex].sharedOwnerPending  = false;
  m_frameDelivery[frameIndex].sharedPending       = true;
  m_frameDelivery[frameIndex].sharedDelivered     = true;
  m_frameResendPending                             = false;
  m_frameScheduler.FrameDelivered(clientIDs, targetCount);
  return true;
}

bool CIndirectDeviceContext::PostSharedOwnerFrame(unsigned frameIndex,
  const CFrameScheduler::Schedule& schedule)
{
  if (frameIndex >= LGMP_Q_FRAME_BUFFER_LEN || !schedule.clientID ||
      lgmpHostQueuePending(m_frameQueue) >= LGMP_Q_FRAME_LEN)
    return false;

  unsigned recipientCount = 0;
  const LGMP_STATUS status = lgmpHostQueuePostForClients(
    m_frameQueue, FrameScheduleToken(schedule), m_frameMemory[frameIndex],
    &schedule.clientID, 1, &recipientCount);
  if (status != LGMP_OK || !recipientCount)
  {
    if (status != LGMP_OK && status != LGMP_ERR_QUEUE_FULL)
      DEBUG_ERROR("Failed to publish shared owner frame: %s",
        lgmpStatusString(status));
    return false;
  }

  FrameDelivery& delivery       = m_frameDelivery[frameIndex];
  delivery.sharedOwnerToken     = FrameScheduleToken(schedule);
  delivery.sharedOwnerClientID  = schedule.clientID;
  delivery.sharedOwnerPending   = true;
  delivery.sharedPending        = true;
  if (!delivery.sharedDelivered)
    m_frameResendPending = true;
  return true;
}

void CIndirectDeviceContext::ProcessFrameDeliveries()
{
  if (!m_frameQueue)
    return;
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    if (!m_frameOwnerQueue[i])
      return;

  AcquireSRWLockExclusive(&m_framePublishLock);

  for (unsigned i = 0; i < LGMP_Q_FRAME_BUFFER_LEN; ++i)
  {
    if (m_frameDelivery[i].sharedOwnerPending &&
        !lgmpHostQueueMessagePending(
          m_frameQueue, m_frameMemory[i],
          m_frameDelivery[i].sharedOwnerToken))
      m_frameDelivery[i].sharedOwnerPending = false;

    if (m_frameDelivery[i].sharedPending &&
        !lgmpHostQueuePayloadPending(m_frameQueue, m_frameMemory[i]))
      m_frameDelivery[i].sharedPending = false;
  }

  CFrameScheduler::Schedule schedule = {};
  const bool scheduled = m_frameScheduler.GetSchedule(schedule);
  const uint64_t scheduleToken = scheduled ?
    FrameScheduleToken(schedule) : 0;
  const LONG publishedFrameIndex =
    m_publishedFrameIndex.load(std::memory_order_acquire);

  int      newestAcked       = -1;
  uint32_t newestAckedClient = 0;
  for (unsigned queueIndex = 0;
       queueIndex < LGMP_Q_FRAME_LEN;
       ++queueIndex)
  {
    OwnerDelivery& owner = m_ownerDelivery[queueIndex];
    if (!owner.active ||
        lgmpHostQueuePayloadPending(
          m_frameOwnerQueue[queueIndex],
          m_frameMemory[owner.frameIndex]))
      continue;

    const unsigned frameIndex = owner.frameIndex;
    const bool latestDelivery =
      static_cast<LONG>(frameIndex) == publishedFrameIndex &&
      m_frameLastPublishSequence[frameIndex] == m_framePublishSequence;
    const bool authoritative = (!scheduled && latestDelivery) ||
      (scheduled && owner.clientID == schedule.clientID &&
       owner.token == scheduleToken);
    if (authoritative &&
        (newestAcked < 0 ||
          m_frameLastPublishSequence[frameIndex] >
            m_frameLastPublishSequence[newestAcked]))
    {
      newestAcked       = static_cast<int>(frameIndex);
      newestAckedClient = owner.clientID;
    }
    else if (!authoritative && !latestDelivery)
      m_frameResendPending = true;

    m_frameDelivery[frameIndex].ownerQueueMask &=
      ~(1U << queueIndex);
    owner = {};
  }

  if (newestAcked >= 0)
  {
    FrameDelivery& delivery = m_frameDelivery[newestAcked];
    const bool needsShared = !delivery.sharedDelivered;

    const bool latest = newestAcked == publishedFrameIndex &&
      m_frameLastPublishSequence[newestAcked] == m_framePublishSequence;
    if (needsShared &&
        (!latest ||
          m_frameInFlight[newestAcked].load(std::memory_order_acquire) ||
          lgmpHostQueuePending(m_frameQueue) != 0))
    {
      m_frameResendPending = true;
    }
    else if (needsShared &&
        !PostSharedFrame(
          static_cast<unsigned>(newestAcked), newestAckedClient))
    {
      m_frameResendPending = true;
    }
  }

  if (m_frameResendPending &&
      lgmpHostQueuePending(m_frameQueue) == 0)
  {
    const LONG frameIndex =
      m_publishedFrameIndex.load(std::memory_order_acquire);
    if (frameIndex >= 0 &&
        !m_frameDelivery[frameIndex].sharedPending &&
        !m_frameInFlight[frameIndex].load(std::memory_order_acquire))
    {
      FrameDelivery& delivery = m_frameDelivery[frameIndex];
      bool ownerDelivered     = !scheduled;
      if (scheduled)
      {
        ownerDelivered =
          delivery.sharedOwnerClientID == schedule.clientID &&
          delivery.sharedOwnerToken == scheduleToken;
        for (const OwnerDelivery& owner : m_ownerDelivery)
          if (owner.active &&
              owner.clientID == schedule.clientID &&
              owner.token == scheduleToken &&
              owner.frameIndex == static_cast<unsigned>(frameIndex))
          {
            ownerDelivered = true;
            break;
          }
      }

      const bool reserveSharedOwner = scheduled &&
        delivery.sharedOwnerClientID == schedule.clientID &&
        delivery.sharedOwnerToken == scheduleToken &&
        FindAvailableOwnerQueue(static_cast<unsigned>(frameIndex)) < 0;
      if (ownerDelivered && !reserveSharedOwner)
      {
        const uint32_t excludeClientID = scheduled ?
          schedule.clientID : delivery.sharedOwnerClientID;
        PostSharedFrame(
          static_cast<unsigned>(frameIndex), excludeClientID);
      }
    }
  }
  ReleaseSRWLockExclusive(&m_framePublishLock);
}

int CIndirectDeviceContext::FindAvailableOwnerQueue(
  unsigned preferredIndex) const
{
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    const unsigned queueIndex =
      (preferredIndex + i) % LGMP_Q_FRAME_LEN;
    if (!m_ownerDelivery[queueIndex].active &&
        m_frameOwnerQueue[queueIndex] &&
        lgmpHostQueuePending(m_frameOwnerQueue[queueIndex]) == 0)
      return static_cast<int>(queueIndex);
  }

  return -1;
}

int CIndirectDeviceContext::FindOwnerDelivery(uint32_t clientID) const
{
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    if (m_ownerDelivery[i].active &&
        m_ownerDelivery[i].clientID == clientID)
      return static_cast<int>(i);

  return -1;
}

int CIndirectDeviceContext::FindSharedOwnerDelivery(
  uint32_t clientID) const
{
  for (unsigned i = 0; i < LGMP_Q_FRAME_BUFFER_LEN; ++i)
    if (m_frameDelivery[i].sharedOwnerPending &&
        m_frameDelivery[i].sharedOwnerClientID == clientID)
      return static_cast<int>(i);

  return -1;
}

bool CIndirectDeviceContext::HasOwnerDelivery(uint32_t clientID) const
{
  return FindOwnerDelivery(clientID) >= 0 ||
    FindSharedOwnerDelivery(clientID) >= 0;
}

int CIndirectDeviceContext::FindAvailableFrameBuffer() const
{
  const LONG publishedFrameIndex =
    m_publishedFrameIndex.load(std::memory_order_acquire);
  int      available     = -1;
  uint64_t newestPublish = 0;
  for (unsigned frameIndex = 0;
       frameIndex < LGMP_Q_FRAME_BUFFER_LEN;
       ++frameIndex)
  {
    if (static_cast<LONG>(frameIndex) == publishedFrameIndex ||
        m_frameInFlight[frameIndex].load(std::memory_order_acquire) ||
        m_frameDelivery[frameIndex].ownerQueueMask ||
        m_frameDelivery[frameIndex].sharedPending ||
        lgmpHostQueuePayloadPending(
          m_frameQueue, m_frameMemory[frameIndex]))
      continue;

    bool ownerPending = false;
    for (unsigned queueIndex = 0;
         !ownerPending && queueIndex < LGMP_Q_FRAME_LEN;
         ++queueIndex)
      ownerPending = lgmpHostQueuePayloadPending(
        m_frameOwnerQueue[queueIndex], m_frameMemory[frameIndex]);

    if (ownerPending)
      continue;

    if (available < 0 ||
        m_frameLastPublishSequence[frameIndex] > newestPublish)
    {
      available     = static_cast<int>(frameIndex);
      newestPublish = m_frameLastPublishSequence[frameIndex];
    }
  }

  return available;
}

bool CIndirectDeviceContext::FrameBufferAvailable(
  const CFrameScheduler::Schedule& schedule)
{
  if (!m_lgmp || !m_frameQueue)
    return false;
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    if (!m_frameOwnerQueue[i])
      return false;

  AcquireSRWLockShared(&m_framePublishLock);
  bool deliveryAvailable;
  if (schedule.clientID)
    deliveryAvailable = !HasOwnerDelivery(schedule.clientID) &&
      (FindAvailableOwnerQueue(0) >= 0 ||
       lgmpHostQueuePending(m_frameQueue) < LGMP_Q_FRAME_LEN);
  else
    deliveryAvailable = lgmpHostQueuePending(m_frameQueue) == 0;

  const bool available = deliveryAvailable &&
    FindAvailableFrameBuffer() >= 0;
  ReleaseSRWLockShared(&m_framePublishLock);
  return available;
}

void CIndirectDeviceContext::ProcessFrameQueue()
{
  if (!m_lgmp)
    return;

  AcquireSRWLockExclusive(&m_lgmpProcessLock);
  const LGMP_STATUS status = lgmpHostProcess(m_lgmp);
  ReleaseSRWLockExclusive(&m_lgmpProcessLock);

  if (status != LGMP_OK && status != LGMP_ERR_CORRUPTED)
    DEBUG_ERROR("lgmpHostProcess Failed: %s", lgmpStatusString(status));

  if (status == LGMP_OK)
    ProcessFrameDeliveries();
}

CIndirectDeviceContext::PreparedFrameBuffer CIndirectDeviceContext::PrepareFrameBuffer(
  unsigned pitch, const D12FrameFormat& srcFormat, const D12FrameFormat& dstFormat,
  const RECT * dirtyRects, unsigned nbDirtyRects)
{
  PreparedFrameBuffer result = {};

  const unsigned dataWidth  = dstFormat.dataWidth ?
    dstFormat.dataWidth : (unsigned)dstFormat.desc.Width;
  const unsigned dataHeight = dstFormat.dataHeight ?
    dstFormat.dataHeight : dstFormat.desc.Height;

  if (dstFormat.format == FRAME_TYPE_INVALID)
  {
    DEBUG_ERROR("Unsupported frame format, skipping frame");
    return result;
  }

  AcquireSRWLockExclusive(&m_framePublishLock);
  const int availableFrameIndex = FindAvailableFrameBuffer();
  bool expected = false;
  const bool acquired = availableFrameIndex >= 0 &&
    m_frameInFlight[availableFrameIndex].compare_exchange_strong(
      expected, true, std::memory_order_acq_rel);
  if (acquired)
    m_frameDelivery[availableFrameIndex] = {};
  const bool fullCopy = acquired &&
    (!m_frameLastPublishSequence[availableFrameIndex] ||
      m_framePublishSequence >
        m_frameLastPublishSequence[availableFrameIndex] + 1);
  ReleaseSRWLockExclusive(&m_framePublishLock);
  if (!acquired)
    return result;
  const unsigned frameIndex = static_cast<unsigned>(availableFrameIndex);

  if (m_width       != dataWidth             ||
      m_height      != dataHeight            ||
      m_frameWidth  != dstFormat.width       ||
      m_frameHeight != dstFormat.height      ||
      m_pitch       != pitch                 ||
      m_format      != dstFormat.desc.Format ||
      m_frameType   != dstFormat.format)
  {
    m_width       = dataWidth;
    m_height      = dataHeight;
    m_frameWidth  = dstFormat.width;
    m_frameHeight = dstFormat.height;
    m_pitch       = pitch;
    m_format      = dstFormat.desc.Format;
    m_frameType   = dstFormat.format;
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

  KVMFRFrame * fi = m_frame[frameIndex];

  const unsigned maxRows = (unsigned)(m_maxFrameSize / pitch);
  const int bpp = dstFormat.format == FRAME_TYPE_RGBA16F ? 8 : 4;
  KVMFRFrameFlags flags =
    (dstFormat.hdr         ? FRAME_FLAG_HDR          : 0) |
    (dstFormat.hdrPQ       ? FRAME_FLAG_HDR_PQ       : 0) |
    (dstFormat.hdrMetadata ? FRAME_FLAG_HDR_METADATA : 0);

  if (maxRows < dataHeight)
    flags |= FRAME_FLAG_TRUNCATED;

  fi->formatVer        = m_formatVer;
  fi->frameSerial      = m_frameSerial++;
  fi->screenWidth      = srcFormat.width;
  fi->screenHeight     = srcFormat.height;
  fi->dataWidth        = dataWidth;
  fi->dataHeight       = min(maxRows, dataHeight);
  fi->frameWidth       = dstFormat.width;
  fi->frameHeight      = dstFormat.height;
  fi->stride           = pitch / bpp;
  fi->pitch            = pitch;
  // fi->offset is initialized at startup
  fi->flags            = flags;
  fi->sdrWhiteLevel    = dstFormat.sdrWhiteLevel;
  fi->captureTime      = 0;
  fi->postProcessTime  = 0;
  fi->copyTime         = 0;
  fi->readyTime        = 0;
  fi->timingSerial     = 0;
  InterlockedExchange((volatile LONG *)&fi->timingValid, 0);
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

  FrameBuffer* fb = m_frameBuffer[frameIndex];
  fb->wp = 0;

  result.frameIndex = frameIndex;
  result.mem        = fb->data;
  result.fullCopy   = fullCopy;

  return result;
}

bool CIndirectDeviceContext::PublishFrameBuffer(unsigned frameIndex,
  const CFrameScheduler::Schedule& schedule)
{
  if (!m_frameQueue || frameIndex >= LGMP_Q_FRAME_BUFFER_LEN)
    return false;

  AcquireSRWLockExclusive(&m_framePublishLock);
  CFrameScheduler::Schedule currentSchedule = {};
  const bool scheduling =
    m_frameScheduler.GetSchedule(currentSchedule);
  if (scheduling != (schedule.clientID != 0) ||
      (scheduling && !FrameScheduleMatches(schedule, currentSchedule)))
  {
    ReleaseSRWLockExclusive(&m_framePublishLock);
    return false;
  }

  LGMP_STATUS status = LGMP_OK;
  bool published     = false;
  if (schedule.clientID)
  {
    if (HasOwnerDelivery(schedule.clientID))
      status = LGMP_ERR_QUEUE_FULL;
    else
    {
      const int ownerQueueIndex = FindAvailableOwnerQueue(frameIndex);
      if (ownerQueueIndex < 0)
        published = PostSharedOwnerFrame(frameIndex, schedule);
      else
      {
        unsigned recipientCount = 0;
        status = lgmpHostQueuePostForClients(
          m_frameOwnerQueue[ownerQueueIndex], FrameScheduleToken(schedule),
          m_frameMemory[frameIndex],
          &schedule.clientID, 1, &recipientCount);
        if (status == LGMP_OK && recipientCount)
        {
          const unsigned queueIndex =
            static_cast<unsigned>(ownerQueueIndex);
          OwnerDelivery& owner = m_ownerDelivery[queueIndex];
          owner.token           = FrameScheduleToken(schedule);
          owner.clientID        = schedule.clientID;
          owner.frameIndex      = frameIndex;
          owner.active          = true;

          FrameDelivery& delivery = m_frameDelivery[frameIndex];
          delivery.ownerQueueMask |= 1U << queueIndex;
          delivery.sharedDelivered = false;
          published                = true;

          if (!PostSharedFrame(frameIndex, schedule.clientID))
            m_frameResendPending = true;
        }
      }
    }
  }
  else
    published = PostSharedFrame(frameIndex, 0);

  if (published)
  {
    m_frameLastPublishSequence[frameIndex] = ++m_framePublishSequence;
    m_publishedFrameIndex.store(
      static_cast<LONG>(frameIndex), std::memory_order_release);
  }
  ReleaseSRWLockExclusive(&m_framePublishLock);

  if (!published)
  {
    if (status != LGMP_OK && status != LGMP_ERR_QUEUE_FULL)
      DEBUG_ERROR("Failed to publish frame: %s",
        lgmpStatusString(status));
    return false;
  }

  return true;
}

bool CIndirectDeviceContext::RepublishFrameBuffer(
  const CFrameScheduler::Schedule& schedule)
{
  if (!schedule.clientID)
    return false;

  AcquireSRWLockExclusive(&m_framePublishLock);
  CFrameScheduler::Schedule currentSchedule = {};
  if (!m_frameScheduler.GetSchedule(currentSchedule) ||
      !FrameScheduleMatches(schedule, currentSchedule))
  {
    ReleaseSRWLockExclusive(&m_framePublishLock);
    return false;
  }

  const LONG frameIndex =
    m_publishedFrameIndex.load(std::memory_order_acquire);
  if (frameIndex < 0 ||
      m_frameInFlight[frameIndex].load(std::memory_order_acquire))
  {
    ReleaseSRWLockExclusive(&m_framePublishLock);
    return false;
  }

  const uint64_t scheduleToken = FrameScheduleToken(schedule);
  const int existingSharedDelivery =
    FindSharedOwnerDelivery(schedule.clientID);
  if (existingSharedDelivery >= 0)
  {
    const FrameDelivery& delivery =
      m_frameDelivery[existingSharedDelivery];
    const bool delivered = existingSharedDelivery == frameIndex &&
      delivery.sharedOwnerToken == scheduleToken;
    const uint32_t frameSerial = m_frame[frameIndex]->frameSerial;
    ReleaseSRWLockExclusive(&m_framePublishLock);
    if (delivered)
      m_frameScheduler.FrameRepublished(schedule, frameSerial);
    return delivered;
  }

  const int existingDelivery =
    FindOwnerDelivery(schedule.clientID);
  if (existingDelivery >= 0)
  {
    const OwnerDelivery& owner = m_ownerDelivery[existingDelivery];
    const bool delivered = owner.frameIndex ==
        static_cast<unsigned>(frameIndex) &&
      owner.token == scheduleToken;
    const uint32_t frameSerial = m_frame[frameIndex]->frameSerial;
    ReleaseSRWLockExclusive(&m_framePublishLock);
    if (delivered)
      m_frameScheduler.FrameRepublished(schedule, frameSerial);
    return delivered;
  }

  const int ownerQueueIndex =
    FindAvailableOwnerQueue(static_cast<unsigned>(frameIndex));
  if (ownerQueueIndex < 0)
  {
    const bool published = PostSharedOwnerFrame(
      static_cast<unsigned>(frameIndex), schedule);
    const uint32_t frameSerial = m_frame[frameIndex]->frameSerial;
    ReleaseSRWLockExclusive(&m_framePublishLock);
    if (published)
      m_frameScheduler.FrameRepublished(schedule, frameSerial);
    return published;
  }

  const uint32_t frameSerial = m_frame[frameIndex]->frameSerial;

  unsigned recipientCount = 0;
  const LGMP_STATUS status = lgmpHostQueuePostForClients(
    m_frameOwnerQueue[ownerQueueIndex], scheduleToken,
    m_frameMemory[frameIndex],
    &schedule.clientID, 1, &recipientCount);
  if (status == LGMP_OK && recipientCount)
  {
    const unsigned queueIndex = static_cast<unsigned>(ownerQueueIndex);
    OwnerDelivery& owner = m_ownerDelivery[queueIndex];
    owner.token           = scheduleToken;
    owner.clientID        = schedule.clientID;
    owner.frameIndex      = static_cast<unsigned>(frameIndex);
    owner.active          = true;

    FrameDelivery& delivery = m_frameDelivery[frameIndex];
    delivery.ownerQueueMask |= 1U << queueIndex;
  }
  ReleaseSRWLockExclusive(&m_framePublishLock);

  if (status != LGMP_OK || !recipientCount)
  {
    if (status != LGMP_OK && status != LGMP_ERR_QUEUE_FULL)
      DEBUG_ERROR("Failed to republish frame: %s",
        lgmpStatusString(status));
    return false;
  }

  m_frameScheduler.FrameRepublished(schedule, frameSerial);
  return true;
}

void CIndirectDeviceContext::CommitFrameBuffer(unsigned frameIndex,
  const CFrameScheduler::Schedule& schedule, bool periodic)
{
  if (frameIndex >= LGMP_Q_FRAME_BUFFER_LEN)
    return;

  m_frameScheduler.FramePublished(
    schedule, m_frame[frameIndex]->frameSerial,
    CFrameScheduler::Nanotime(), periodic);
}

void CIndirectDeviceContext::ObserveFrame(uint64_t now)
{
  m_frameScheduler.ObserveFrame(now);
}

void CIndirectDeviceContext::ForceFrame()
{
  m_frameScheduler.ForceFrame();
}

bool CIndirectDeviceContext::GetPublishTarget(uint64_t now,
  uint64_t& target, CFrameScheduler::Schedule& schedule, bool& periodic,
  bool& republish)
{
  return m_frameScheduler.GetPublishTarget(
    now, target, schedule, periodic, republish);
}

void CIndirectDeviceContext::FrameSuperseded()
{
  m_frameScheduler.FrameSuperseded();
}

void CIndirectDeviceContext::RecordFrameTiming(uint64_t duration)
{
  m_frameScheduler.RecordFrameTiming(duration);
}

void CIndirectDeviceContext::AbortFrameBuffer(unsigned frameIndex)
{
  if (frameIndex >= LGMP_Q_FRAME_BUFFER_LEN)
    return;

  AcquireSRWLockExclusive(&m_framePublishLock);
  m_frameBuffer[frameIndex]->wp = 0;
  InterlockedExchange(
    (volatile LONG *)&m_frame[frameIndex]->timingValid, 0);
  m_frameInFlight[frameIndex].store(false, std::memory_order_release);
  ReleaseSRWLockExclusive(&m_framePublishLock);
}

void CIndirectDeviceContext::FailFrameBuffer(unsigned frameIndex)
{
  if (frameIndex >= LGMP_Q_FRAME_BUFFER_LEN)
    return;

  InterlockedExchange((volatile LONG *)&m_frame[frameIndex]->timingValid, 0);
  FinalizeFrameBuffer(frameIndex);
  CompleteFrameBuffer(frameIndex);
}

void CIndirectDeviceContext::CompleteFrameBuffer(unsigned frameIndex)
{
  if (frameIndex < LGMP_Q_FRAME_BUFFER_LEN)
    m_frameInFlight[frameIndex].store(false, std::memory_order_release);
}

void CIndirectDeviceContext::SetFrameTiming(unsigned frameIndex,
  uint64_t captureTime, uint64_t postProcessTime, uint64_t copyTime,
  uint64_t readyTime)
{
  if (frameIndex >= LGMP_Q_FRAME_BUFFER_LEN)
    return;

  KVMFRFrame * frame      = m_frame[frameIndex];
  frame->captureTime      = captureTime;
  frame->postProcessTime  = postProcessTime;
  frame->copyTime         = copyTime;
  frame->readyTime        = readyTime;
  frame->timingSerial     = frame->frameSerial;
  InterlockedExchange((volatile LONG *)&frame->timingValid, 1);
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
  const KVMFRFrame * frame = m_frame[frameIndex];
  FrameBuffer      * fb    = m_frameBuffer[frameIndex];
  fb->wp = frame->dataHeight * frame->pitch;
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
