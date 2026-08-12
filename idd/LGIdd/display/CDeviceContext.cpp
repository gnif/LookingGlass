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

#include "display/CDeviceContext.h"

#include "display/IddCxCompat.h"
#include "ipc/CInputPipeServer.h"
#include "ipc/CPipeServer.h"
#include "transport/IFrameTransport.h"
#include "transport/IInputTransport.h"
#include "transport/TransportFactory.h"
#include "CDebug.h"

#include <dxgi1_2.h>
#include <utility>

// Adapter and monitor lifecycle

static const UINT IDDCX_VERSION_1_10 = 0x1A00;

CDeviceContext::CDeviceContext(WDFDEVICE wdfDevice) :
  m_wdfDevice(wdfDevice),
  m_transport(CreateTransport()),
  m_displayConfiguration(g_settings)
{
}

CDeviceContext::~CDeviceContext()
{
  // These callbacks dereference this context. Drain them before the subsystem
  // members are destroyed in frame, control, host order.
  if (m_recoveryHandlerSet)
  {
    g_pipe.ClearRecoveryHandler(this);
    m_recoveryHandlerSet = false;
  }

  if (m_initTimer)
  {
    WdfTimerStop(m_initTimer, TRUE);
    m_initTimer = nullptr;
  }

  if (m_transportTimer)
  {
    WdfTimerStop(m_transportTimer, TRUE);
    m_transportTimer = nullptr;
  }

  if (m_transport)
    m_transport->Stop();
}

void CDeviceContext::QueryIddCxCapabilities()
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
    IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2) &&
    IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorQueryHardwareCursor3) &&
    IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorUpdateModes2) &&
    IDD_IS_FIELD_AVAILABLE(
      IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo) &&
    IDD_IS_FIELD_AVAILABLE(
      IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterCommitModes2) &&
    IDD_IS_FIELD_AVAILABLE(
      IDD_CX_CLIENT_CONFIG, EvtIddCxParseMonitorDescription2) &&
    IDD_IS_FIELD_AVAILABLE(
      IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorQueryTargetModes2) &&
    IDD_IS_FIELD_AVAILABLE(
      IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorSetDefaultHdrMetaData) &&
    IDD_IS_FIELD_AVAILABLE(
      IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorSetGammaRamp);
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

void CDeviceContext::ScheduleInitRetry()
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
        auto wrapper = WdfObjectGet_CDeviceContextWrapper(parent);
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

void CDeviceContext::StopInitRetry()
{
  if (m_initTimer)
    WdfTimerStop(m_initTimer, FALSE);
}

void CDeviceContext::InitAdapter()
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

  // At boot the selected transport may not be available yet. Rather than
  // silently abandoning the adapter (leaving the device loaded but with no
  // monitor), retry from a timer until it can be opened.
  if (!m_transportOpened)
  {
    if (!m_transport)
    {
      DEBUG_ERROR("Failed to create the frame transport");
      m_initInProgress.store(0);
      return;
    }

    const ITransport::OpenResult result = m_transport->Open();
    if (result != ITransport::OpenResult::SUCCESS)
    {
      if (result == ITransport::OpenResult::RETRY)
      {
        DEBUG_WARN("Frame transport not available yet, scheduling init retry");
        ScheduleInitRetry();
      }
      else
        DEBUG_ERROR("Failed to open the frame transport");
      m_initInProgress.store(0);
      return;
    }
    m_transportOpened = true;
  }

  // Select the render adapter before advertising capabilities. If no hardware
  // adapter is available, this is a software-rendered display and must remain
  // SDR-only; the software path must never depend on compute processing.
  m_havePreferredRenderAdapter = false;
  m_preferredRenderAdapter     = {};
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
      m_preferredRenderAdapter     = adapterDesc.AdapterLuid;
      m_havePreferredRenderAdapter = true;
      break;
    }

    factory->Release();
  }

  m_softwareMode = !m_havePreferredRenderAdapter;
  if (m_softwareMode)
    DEBUG_INFO("No hardware render adapter available; using SDR software mode");

  QueryIddCxCapabilities();
  DEBUG_TRACE("Initializing frame transport metadata");
  if (!InitializeTransport())
  {
    m_initInProgress.store(0);
    return;
  }
  DEBUG_TRACE("Loading configured display modes");
  if (!m_displayConfiguration.Load(m_transport->GetMemoryLimits()))
  {
    m_initInProgress.store(0);
    return;
  }
  DEBUG_TRACE("Initializing monitor EDID");
  m_displayConfiguration.InitializeEdid(CanProcessFP16());

  const CDisplayConfiguration::Description description =
    m_displayConfiguration.GetDescription();
  DEBUG_INFO("Initializing adapter with %llu modes and a %u-byte EDID",
    (unsigned long long)description.modeCount,
    (UINT)description.edid.size());

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

  caps.MaxMonitorsSupported            = 1;
  caps.StaticDesktopReencodeFrameCount = 1;

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
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, CDeviceContextWrapper);

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
    m_displayConfiguration.RebuildEdid(false);
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

  auto * wrapper = WdfObjectGet_CDeviceContextWrapper(m_adapter);
  wrapper->context = this;
  DEBUG_INFO("IddCxAdapterInitAsync started successfully (adapter %p)",
    m_adapter);
  DEBUG_INFO("Adapter context attached; waiting for initialization callback");

  // Adapter is up; no need to keep retrying.
  StopInitRetry();
  m_initInProgress.store(0);
  DEBUG_INFO("Adapter initialization request complete; returning to IddCx");
}

void CDeviceContext::FinishAdapterInit(UINT connectorIndex)
{
  // Try to co-exist with the virtual video device by telling IddCx which
  // hardware adapter we prefer to render on. Do this only after the adapter
  // has finished initializing, but before adding its monitor.
  if (m_havePreferredRenderAdapter)
  {
    IDARG_IN_ADAPTERSETRENDERADAPTER args = {};
    args.PreferredRenderAdapter = m_preferredRenderAdapter;
    IddCxAdapterSetRenderAdapter(m_adapter, &args);
    DEBUG_INFO("Preferred render adapter set");
  }

  FinishInit(connectorIndex);
}

void CDeviceContext::FinishInit(UINT connectorIndex)
{
  CDisplayConfiguration::Description description =
    m_displayConfiguration.GetDescription();
  if (m_monitorManager.Create(
      connectorIndex, m_adapter, std::move(description.edid), this))
    m_transport->SyncRecovery();
}

void CDeviceContext::ReplugMonitor()
{
  if (m_monitorManager.Replug() ==
      CMonitorManager::ReplugAction::CREATE)
    FinishInit(0);
}

void CDeviceContext::ReloadSettings()
{
  if (!m_displayConfiguration.ReloadSettings(
      m_transport->GetMemoryLimits()))
    return;

  ReplugMonitor();
}

void CDeviceContext::OnMonitorDestroyed(IDDCX_MONITOR monitor)
{
  m_monitorManager.OnDestroyed(monitor);
}

void CDeviceContext::OnSwapChainAssigned()
{
  m_monitorManager.OnSwapChainAssigned();
}

void CDeviceContext::OnSwapChainReleased()
{
  m_monitorManager.OnSwapChainReleased();
}

void CDeviceContext::OnSwapChainReady()
{
  const CMonitorManager::ReadyAction action =
    m_monitorManager.OnSwapChainReady();

  // Do not expose the context to pipe reload requests until the initial swap
  // chain has reached the same ready state used by the replug gate.
  g_pipe.SetDeviceContext(this);

  if (action.replug)
    m_monitorManager.QueueReplug();
  else if (action.setMode)
    g_pipe.SetDisplayMode(
      action.mode.width, action.mode.height, action.mode.refreshMilliHz);
}

// Display configuration

void CDeviceContext::SetResolution(uint32_t width, uint32_t height)
{
  const CDisplayConfiguration::ResolutionResult result =
    m_displayConfiguration.SetResolution(
      width, height, m_transport->GetMemoryLimits());

  switch (result.status)
  {
    case CDisplayConfiguration::ResolutionStatus::SUCCESS:
      m_monitorManager.RequestMode(result.mode);
      // IddCxMonitorUpdateModes[2] does not invalidate Windows' cached mode
      // list, so depart and re-arrive the monitor to rebuild the topology.
      ReplugMonitor();
      break;

    case CDisplayConfiguration::ResolutionStatus::TOO_LARGE:
      g_pipe.ResolutionRejected(width, height, result.requiredMiB);
      break;

    default:
      break;
  }
}

// Frame transport

bool CDeviceContext::InitializeTransport()
{
  if (!m_transport)
    return false;

  if (m_transportTimer)
    return true;

  g_pipe.SetRecoveryHandler(
    [](void * opaque, uint64_t route, uint64_t session,
       uint32_t serial, bool active, LGPipeMsg::Type result)
    {
      CDeviceContext * context =
        static_cast<CDeviceContext *>(opaque);

      SourceKey source;
      {
        CSRWExclusiveLock routeLock(context->m_recoveryRouteLock);
        if (!route || route != context->m_recoveryRoute ||
            session != context->m_recoverySession ||
            serial != context->m_recoverySerial ||
            active != context->m_recoveryActive)
        {
          DEBUG_WARN("Ignoring stale recovery route");
          return;
        }
        source = context->m_recoverySource;
        context->m_recoveryRoute   = 0;
        context->m_recoverySource  = {};
        context->m_recoverySession = 0;
        context->m_recoverySerial  = 0;
        context->m_recoveryActive  = false;
      }

      ITransport::Recovery state = ITransport::Recovery::FAILED;
      uint32_t error = ERROR_SUCCESS;
      switch (result)
      {
        case LGPipeMsg::RECOVERY_OFF:
          state = ITransport::Recovery::NORMAL;
          break;

        case LGPipeMsg::RECOVERY_ON:
          state = ITransport::Recovery::ACTIVE;
          break;

        case LGPipeMsg::RECOVERY_FAILED:
          error = ERROR_GEN_FAILURE;
          break;

        case LGPipeMsg::RECOVERY_NO_DISPLAY:
          error = ERROR_NOT_FOUND;
          break;

        default:
          return;
      }

      context->m_transport->RecoveryStatus(
        source, session, serial, active, state, error);
    },
    this);
  m_recoveryHandlerSet = true;

  // Claim the pipe recovery channel before initializing the producer session
  // so no request cached by a prior device context can cross the handoff.
  if (!m_transport->Initialize())
  {
    g_pipe.ClearRecoveryHandler(this);
    m_recoveryHandlerSet = false;
    return false;
  }

  WDF_TIMER_CONFIG config;
  WDF_TIMER_CONFIG_INIT_PERIODIC(&config,
    [](WDFTIMER timer) -> void
    {
      WDFOBJECT parent = WdfTimerGetParentObject(timer);
      auto wrapper = WdfObjectGet_CDeviceContextWrapper(parent);
      wrapper->context->TransportTimer();
    },
    10);
  config.AutomaticSerialization = FALSE;

  /**
   * Documentation states that Dispatch is not available under UMDF,
   * however using Passive returns a not-supported error and Dispatch works.
   */
  WDF_OBJECT_ATTRIBUTES attribs;
  WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
  attribs.ParentObject   = m_wdfDevice;
  attribs.ExecutionLevel = WdfExecutionLevelDispatch;

  NTSTATUS status = WdfTimerCreate(
    &config, &attribs, &m_transportTimer);
  if (!NT_SUCCESS(status))
  {
    g_pipe.ClearRecoveryHandler(this);
    m_recoveryHandlerSet = false;
    DEBUG_ERROR_HR(status, "Transport timer creation failed");
    return false;
  }

  WdfTimerStart(m_transportTimer, WDF_REL_TIMEOUT_IN_MS(10));
  return true;
}

bool CDeviceContext::SetupTransport(size_t alignSize)
{
  // Frame buffers cannot be allocated until the GPU-specific alignment is
  // known. The swap-chain path may call this again after setup completed.
  if (!m_transport->Frames().GetMaxFrameSize())
  {
    if (!InitializeTransport() || !m_transport->Setup(alignSize))
      return false;
  }

  if (!m_transport->Input().Start(g_inputPipeServer))
  {
    DEBUG_ERROR("Failed to start input transport");
    return false;
  }

  return true;
}

void CDeviceContext::TransportTimer()
{
  // Monitor work is deferred off IddCx callback threads.
  switch (m_monitorManager.TakeDeferredAction())
  {
    case CMonitorManager::DeferredAction::CREATE:
      FinishInit(0);
      return;

    case CMonitorManager::DeferredAction::REPLUG:
      ReplugMonitor();
      return;

    case CMonitorManager::DeferredAction::NONE:
      break;
  }

  m_transport->Process(*this);
}

void CDeviceContext::OnSetCursorPos(
  const SourceKey& source, int32_t x, int32_t y)
{
  UNREFERENCED_PARAMETER(source);
  g_pipe.SetCursorPos(x, y);
}

void CDeviceContext::OnSetResolution(const SourceKey& source,
  uint32_t width, uint32_t height)
{
  UNREFERENCED_PARAMETER(source);
  SetResolution(width, height);
}

void CDeviceContext::OnRecoveryRequest(const SourceKey& source,
  uint64_t session, uint32_t serial, bool active)
{
  if (!source.backend || !source.epoch || !session || !serial ||
      (serial & LGPipeMsg::RECOVERY_ACTIVE))
    return;

  CSRWExclusiveLock publishLock(m_recoveryPublishLock);
  uint64_t route;
  {
    CSRWExclusiveLock routeLock(m_recoveryRouteLock);
    route = m_nextRecoveryRoute++;
    if (!m_nextRecoveryRoute)
      ++m_nextRecoveryRoute;

    m_recoveryRoute   = route;
    m_recoverySource  = source;
    m_recoverySession = session;
    m_recoverySerial  = serial;
    m_recoveryActive  = active;
  }

  if (!g_pipe.SetRecovery(this, route, session, serial, active))
  {
    CSRWExclusiveLock routeLock(m_recoveryRouteLock);
    if (m_recoveryRoute != route)
      return;
    m_recoveryRoute   = 0;
    m_recoverySource  = {};
    m_recoverySession = 0;
    m_recoverySerial  = 0;
    m_recoveryActive  = false;
  }
}
