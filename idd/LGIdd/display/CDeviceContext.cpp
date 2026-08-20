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
#include "Atomic.h"
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
  if (!Atomic::CAS(m_initInProgress, initExpected, 1))
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
      Atomic::Store(m_initInProgress, 0);
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
      Atomic::Store(m_initInProgress, 0);
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
    Atomic::Store(m_initInProgress, 0);
    return;
  }
  DEBUG_TRACE("Loading configured display modes");
  if (!m_displayConfiguration.Load(*m_transport))
  {
    Atomic::Store(m_initInProgress, 0);
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
    Atomic::Store(m_initInProgress, 0);
    return;
  }

  m_adapter = initOut.AdapterObject;
  if (!m_adapter)
  {
    DEBUG_ERROR("IddCxAdapterInitAsync succeeded without returning an adapter object");
    Atomic::Store(m_initInProgress, 0);
    return;
  }

  auto * wrapper = WdfObjectGet_CDeviceContextWrapper(m_adapter);
  wrapper->context = this;
  DEBUG_INFO("IddCxAdapterInitAsync started successfully (adapter %p)",
    m_adapter);
  DEBUG_INFO("Adapter context attached; waiting for initialization callback");

  // Adapter is up; no need to keep retrying.
  StopInitRetry();
  Atomic::Store(m_initInProgress, 0);
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

  bool monitorDisabled;
  bool arrivalPending;
  {
    CSRWExclusiveLock lock(m_localRecoveryLock);
    m_adapterReady   = true;
    monitorDisabled = m_recoveryMonitorDisabled;
    arrivalPending  = m_localRecoveryArrival;
  }

  if (!monitorDisabled)
  {
    FinishInit(connectorIndex);
    return;
  }

  if (arrivalPending)
    m_monitorManager.Enable();
  else
  {
    // Recovery can intentionally disable the monitor before the adapter's
    // first arrival. This is still a valid synchronization boundary: allow a
    // later NORMAL request to re-enable the monitor instead of waiting for an
    // arrival that ACTIVE deliberately suppressed.
    m_transport->SyncRecovery();
  }
}

void CDeviceContext::FinishInit(UINT connectorIndex)
{
  CDisplayConfiguration::Description description =
    m_displayConfiguration.GetDescription();
  const bool arrived = m_monitorManager.Create(
    connectorIndex, m_adapter, std::move(description.edid), this);
  if (arrived)
    m_transport->SyncRecovery();
  CompleteRecoveryArrival(arrived);
}

void CDeviceContext::ReplugMonitor()
{
  if (m_monitorManager.Replug() ==
      CMonitorManager::ReplugAction::CREATE)
    FinishInit(0);
}

void CDeviceContext::ReloadSettings()
{
  if (!m_displayConfiguration.ReloadSettings(*m_transport))
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
      action.mode.width, action.mode.height, action.mode.refresh100uHz);
}

// Display configuration

InteractionResult CDeviceContext::SetResolution(
  uint32_t width, uint32_t height)
{
  const CDisplayConfiguration::ResolutionResult result =
    m_displayConfiguration.SetResolution(
      width, height, *m_transport);

  switch (result.status)
  {
    case CDisplayConfiguration::ResolutionStatus::SUCCESS:
      m_monitorManager.RequestMode(result.mode);
      // IddCxMonitorUpdateModes[2] does not invalidate Windows' cached mode
      // list, so depart and re-arrive the monitor to rebuild the topology.
      ReplugMonitor();
      return InteractionResult::ACCEPTED;

    case CDisplayConfiguration::ResolutionStatus::TOO_LARGE:
      g_pipe.ResolutionRejected(width, height, result.requiredMiB);
      return InteractionResult::REJECTED;

    case CDisplayConfiguration::ResolutionStatus::UNSUPPORTED:
      g_pipe.ResolutionRejected(width, height, 0);
      return InteractionResult::REJECTED;

    case CDisplayConfiguration::ResolutionStatus::INVALID:
      return InteractionResult::REJECTED;

    default:
      return InteractionResult::FAILED;
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
       uint32_t serial, bool active, CPipeServer::RecoveryResult result)
    {
      CDeviceContext * context =
        static_cast<CDeviceContext *>(opaque);

      RecoveryAction action;
      action.route   = route;
      action.session = session;
      action.serial  = serial;
      action.active  = active;

      if (result == CPipeServer::RecoveryResult::HELPER_UNAVAILABLE)
      {
        // Pipe recovery messages do not carry the local operation deadline.
        // Recover it from the canonical action so a late monitor transition
        // cannot complete an operation after the recovery hub timed it out.
        {
          CSRWSharedLock lock(context->m_localRecoveryLock);
          if (context->m_latestRecoveryValid &&
              context->SameRecoveryAction(action,
                context->m_latestRecoveryAction))
            action = context->m_latestRecoveryAction;
        }

        if (!context->QueueLocalRecovery(action))
          context->m_transport->RecoveryStatus(
            route, session, serial, active,
            ITransport::Recovery::FAILED, RPC_S_SERVER_UNAVAILABLE);
        return;
      }

      std::lock_guard<std::mutex> transitionLock(
        context->m_recoveryTransitionMutex);
      context->m_helperRecoveryAction   = action;
      context->m_helperRecoveryComplete = true;
      context->RemoveLocalRecovery(action);

      ITransport::Recovery state = ITransport::Recovery::FAILED;
      uint32_t error = ERROR_SUCCESS;
      switch (result)
      {
        case CPipeServer::RecoveryResult::NORMAL:
          state = ITransport::Recovery::NORMAL;
          {
            CSRWExclusiveLock lock(context->m_localRecoveryLock);
            if (context->m_latestRecoveryValid &&
                context->SameRecoveryAction(action,
                  context->m_latestRecoveryAction))
            {
              context->m_recoveryActive = false;
              context->m_latestRecoveryAction.deadline = 0;
            }
          }
          break;

        case CPipeServer::RecoveryResult::ACTIVE:
          state = ITransport::Recovery::ACTIVE;
          {
            CSRWExclusiveLock lock(context->m_localRecoveryLock);
            if (context->m_latestRecoveryValid &&
                context->SameRecoveryAction(action,
                  context->m_latestRecoveryAction))
            {
              context->m_recoveryActive = true;
              context->m_latestRecoveryAction.deadline = 0;
            }
          }
          break;

        case CPipeServer::RecoveryResult::FAILED:
          error = ERROR_GEN_FAILURE;
          break;

        case CPipeServer::RecoveryResult::NO_DISPLAY:
          error = ERROR_NOT_FOUND;
          break;

        default:
          return;
      }

      context->m_transport->RecoveryStatus(
        route, session, serial, active, state, error);
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
  ProcessLocalRecovery();

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

InteractionResult CDeviceContext::OnSetCursorPos(
  const SourceKey& source, int32_t x, int32_t y)
{
  UNREFERENCED_PARAMETER(source);
  return g_pipe.SetCursorPos(x, y) ?
    InteractionResult::ACCEPTED : InteractionResult::UNAVAILABLE;
}

InteractionResult CDeviceContext::OnSetResolution(const SourceKey& source,
  uint32_t width, uint32_t height)
{
  UNREFERENCED_PARAMETER(source);
  return SetResolution(width, height);
}

bool CDeviceContext::OnRecoveryAction(const RecoveryAction& action)
{
  bool recoveryActive;
  bool monitorDisabled;
  {
    std::lock_guard<std::mutex> transitionLock(m_recoveryTransitionMutex);
    CSRWExclusiveLock lock(m_localRecoveryLock);
    m_latestRecoveryAction = action;
    m_latestRecoveryValid  = true;
    recoveryActive         = m_recoveryActive;
    monitorDisabled        = m_recoveryMonitorDisabled;
  }

  // The monitor must exist before Helper can restore the LG display path.
  // Re-arrive it first when recovery previously used the IDD-only fallback.
  if (!action.active && monitorDisabled)
    return QueueLocalRecovery(action);

  const CPipeServer::RecoveryDispatch dispatch = g_pipe.SetRecovery(
    this, action.route, action.session, action.serial, action.active,
    action.active || !recoveryActive);

  std::lock_guard<std::mutex> transitionLock(m_recoveryTransitionMutex);
  if (m_helperRecoveryComplete &&
      SameRecoveryAction(m_helperRecoveryAction, action))
    return true;

  switch (dispatch)
  {
    case CPipeServer::RecoveryDispatch::SENT:
      return true;

    case CPipeServer::RecoveryDispatch::UNAVAILABLE:
    case CPipeServer::RecoveryDispatch::QUEUED:
      return QueueLocalRecovery(action);

    case CPipeServer::RecoveryDispatch::REJECTED:
      return false;
  }
  return false;
}

bool CDeviceContext::SameRecoveryAction(
  const RecoveryAction& left, const RecoveryAction& right)
{
  return left.route == right.route &&
    left.session == right.session &&
    left.serial == right.serial &&
    left.active == right.active;
}

bool CDeviceContext::QueueLocalRecovery(const RecoveryAction& action)
{
  if (!action.route || !action.session || !action.serial)
    return false;

  CSRWExclusiveLock lock(m_localRecoveryLock);
  if (m_latestRecoveryValid &&
      !SameRecoveryAction(action, m_latestRecoveryAction))
    return true;

  if (m_localRecoveryArrival)
  {
    const RecoveryAction& pending = m_localRecoveryArrivalAction;
    if (SameRecoveryAction(pending, action))
      return true;
  }

  for (const RecoveryAction& pending : m_localRecoveryQueue)
    if (SameRecoveryAction(pending, action))
      return true;

  m_localRecoveryQueue.push_back(action);
  return true;
}

void CDeviceContext::RemoveLocalRecovery(const RecoveryAction& action)
{
  CSRWExclusiveLock lock(m_localRecoveryLock);
  for (auto it = m_localRecoveryQueue.begin();
       it != m_localRecoveryQueue.end();)
  {
    if (SameRecoveryAction(*it, action))
      it = m_localRecoveryQueue.erase(it);
    else
      ++it;
  }
}

void CDeviceContext::ProcessLocalRecovery()
{
  std::lock_guard<std::mutex> transitionLock(m_recoveryTransitionMutex);

  RecoveryAction action;
  bool haveAction            = false;
  bool reconcileActive       = false;
  bool reconcileAdapterReady = false;
  {
    CSRWExclusiveLock lock(m_localRecoveryLock);
    for (;;)
    {
      if (m_localRecoveryQueue.empty())
        break;
      action = m_localRecoveryQueue.front();
      m_localRecoveryQueue.pop_front();
      const bool current = !m_latestRecoveryValid ||
        SameRecoveryAction(action, m_latestRecoveryAction);
      const bool expired = action.deadline &&
        GetTickCount64() >= action.deadline;
      if (current && !expired)
      {
        haveAction = true;
        break;
      }

      // The request can expire after Helper disappears but before this timer
      // consumes the handoff. Its result is stale, but an already-established
      // ACTIVE state still needs the IDD monitor physically disabled.
      if (current && expired && m_recoveryActive &&
          !m_recoveryMonitorDisabled)
      {
        m_recoveryMonitorDisabled = true;
        reconcileActive           = true;
        reconcileAdapterReady     = m_adapterReady;
        break;
      }
    }
  }

  if (reconcileActive)
  {
    DEBUG_WARN(
      "IDD Helper is unavailable; reconciling the active recovery topology");
    if (!m_monitorManager.Disable())
    {
      CSRWExclusiveLock lock(m_localRecoveryLock);
      m_recoveryMonitorDisabled = false;
    }
    else if (reconcileAdapterReady)
      m_transport->SyncRecovery();
    return;
  }

  if (!haveAction)
    return;

  if (action.active)
  {
    DEBUG_WARN(
      "IDD Helper is unavailable; disabling the virtual monitor for recovery");
    bool oldRecoveryActive;
    bool oldMonitorDisabled;
    bool adapterReady;
    {
      CSRWExclusiveLock lock(m_localRecoveryLock);
      oldRecoveryActive           = m_recoveryActive;
      oldMonitorDisabled          = m_recoveryMonitorDisabled;
      m_recoveryActive            = true;
      m_recoveryMonitorDisabled   = true;
      adapterReady                = m_adapterReady;
    }

    const bool disabled = m_monitorManager.Disable();
    if (!disabled)
    {
      CSRWExclusiveLock lock(m_localRecoveryLock);
      m_recoveryActive          = oldRecoveryActive;
      m_recoveryMonitorDisabled = oldMonitorDisabled;
    }
    if (disabled && adapterReady)
      m_transport->SyncRecovery();
    m_transport->RecoveryStatus(action.route, action.session, action.serial,
      true, disabled ? ITransport::Recovery::ACTIVE :
      ITransport::Recovery::FAILED,
      disabled ? ERROR_SUCCESS : ERROR_GEN_FAILURE);
    return;
  }

  bool disable = false;
  {
    CSRWExclusiveLock lock(m_localRecoveryLock);
    disable = m_recoveryActive && !m_recoveryMonitorDisabled;
    if (disable)
      m_recoveryMonitorDisabled = true;
  }

  if (disable)
  {
    DEBUG_WARN(
      "IDD Helper disconnected during recovery; cycling the virtual monitor");
    if (!m_monitorManager.Disable())
    {
      {
        CSRWExclusiveLock lock(m_localRecoveryLock);
        m_recoveryMonitorDisabled = false;
      }
      m_transport->RecoveryStatus(action.route, action.session, action.serial,
        false, ITransport::Recovery::FAILED, ERROR_GEN_FAILURE);
      return;
    }
  }

  bool enable      = false;
  bool waitArrival = false;
  {
    CSRWExclusiveLock lock(m_localRecoveryLock);
    if (m_recoveryMonitorDisabled)
    {
      m_localRecoveryArrivalAction = action;
      m_localRecoveryArrival       = true;
      enable                       = m_adapterReady;
      waitArrival                  = true;
    }
    else
      m_recoveryActive = false;
  }

  if (enable)
    m_monitorManager.Enable();

  if (waitArrival)
    return;

  // The normal monitor is already present. With no Helper there is no
  // user-session topology work to perform, so monitor presence is the local
  // completion boundary.
  m_transport->RecoveryStatus(action.route, action.session, action.serial,
    false, ITransport::Recovery::NORMAL, ERROR_SUCCESS);
}

void CDeviceContext::CompleteRecoveryArrival(bool arrived)
{
  RecoveryAction action;
  RecoveryAction latest;
  bool stale         = false;
  bool latestValid   = false;
  bool latestHandled = false;
  bool latestCurrent = false;
  std::unique_lock<std::mutex> transitionLock(m_recoveryTransitionMutex);
  {
    CSRWExclusiveLock lock(m_localRecoveryLock);
    if (!m_localRecoveryArrival)
      return;

    action = m_localRecoveryArrivalAction;
    const uint64_t now        = GetTickCount64();
    const bool     expired    = action.deadline && now >= action.deadline;
    const bool     superseded = m_latestRecoveryValid &&
      !SameRecoveryAction(action, m_latestRecoveryAction);
    stale = expired || superseded;
    if (stale)
    {
      latestValid   = m_latestRecoveryValid;
      latest        = m_latestRecoveryAction;
      latestHandled = m_helperRecoveryComplete &&
        SameRecoveryAction(m_helperRecoveryAction, latest);
      latestCurrent = latestValid &&
        (!latest.deadline || now < latest.deadline);
      if (superseded && arrived && latestCurrent && !latest.active)
      {
        action = latest;
        m_localRecoveryArrivalAction = latest;
        stale = false;
        for (auto it = m_localRecoveryQueue.begin();
             it != m_localRecoveryQueue.end();)
        {
          if (SameRecoveryAction(*it, latest))
            it = m_localRecoveryQueue.erase(it);
          else
            ++it;
        }
      }
      else
      {
        m_localRecoveryArrivalAction = {};
        m_localRecoveryArrival       = false;
      }
    }

    if (arrived && !stale)
    {
      // Arrival is the IDD-only NORMAL completion boundary. Keep the monitor
      // present if Helper disappears while applying its user-session topology.
      m_localRecoveryArrivalAction = {};
      m_localRecoveryArrival       = false;
      m_recoveryActive             = false;
      m_recoveryMonitorDisabled    = false;
    }
    else if (!stale)
    {
      m_localRecoveryArrivalAction = {};
      m_localRecoveryArrival       = false;
    }
  }

  if (stale)
  {
    // The expired/superseded NORMAL operation must not leave the IDD monitor
    // arrived while CPipe still retains the preceding ACTIVE topology. Return
    // to that stable local state before allowing newer work to proceed.
    const bool disabled = m_monitorManager.Disable();
    {
      CSRWExclusiveLock lock(m_localRecoveryLock);
      m_recoveryActive          = true;
      m_recoveryMonitorDisabled = disabled;
    }

    const bool queueLatest = latestValid && latest.active &&
      latestCurrent && !latestHandled;
    transitionLock.unlock();
    if (queueLatest)
      QueueLocalRecovery(latest);
    return;
  }

  if (!arrived)
  {
    const bool disabled = m_monitorManager.Disable();
    {
      CSRWExclusiveLock lock(m_localRecoveryLock);
      if (disabled)
      {
        m_recoveryActive          = true;
        m_recoveryMonitorDisabled = true;
      }
    }
    transitionLock.unlock();
    m_transport->RecoveryStatus(action.route, action.session, action.serial,
      false, ITransport::Recovery::FAILED, ERROR_GEN_FAILURE);
    return;
  }

  transitionLock.unlock();

  // If Helper appeared while the monitor was disabled, let it restore the
  // user-session topology now that the LG display path exists again.
  const CPipeServer::RecoveryDispatch dispatch = g_pipe.SetRecovery(
    this, action.route, action.session, action.serial, false, true);

  {
    std::lock_guard<std::mutex> completionLock(m_recoveryTransitionMutex);
    CSRWExclusiveLock lock(m_localRecoveryLock);
    if (m_helperRecoveryComplete &&
        SameRecoveryAction(m_helperRecoveryAction, action))
      return;
  }

  if (dispatch == CPipeServer::RecoveryDispatch::SENT)
    return;

  m_transport->RecoveryStatus(action.route, action.session, action.serial,
    false,
    (dispatch == CPipeServer::RecoveryDispatch::QUEUED ||
     dispatch == CPipeServer::RecoveryDispatch::UNAVAILABLE) ?
      ITransport::Recovery::NORMAL : ITransport::Recovery::FAILED,
    (dispatch == CPipeServer::RecoveryDispatch::QUEUED ||
     dispatch == CPipeServer::RecoveryDispatch::UNAVAILABLE) ?
      ERROR_SUCCESS : RPC_S_SERVER_UNAVAILABLE);
}
