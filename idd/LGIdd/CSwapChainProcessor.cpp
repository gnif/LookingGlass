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

#include "CSwapChainProcessor.h"
#include "CIndirectMonitorContext.h"
#include "CPlatformInfo.h"

#include <avrt.h>
#include <new>
#include "CDebug.h"
#include "CPipeServer.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
  #define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static const uint32_t HDR_PQ_MIN_LUMINANCE = 50;
static const uint32_t HDR_PQ_MAX_LUMINANCE = 10000;
static const uint64_t PUBLISH_RETRY_NS      = 1000000ULL;

static_assert(LGMP_Q_FRAME_LEN == 2,
  "IDD candidate pipeline assumes two slots");

class CSRWExclusiveLock
{
private:
  SRWLOCK * m_lock;

public:
  explicit CSRWExclusiveLock(SRWLOCK * lock) : m_lock(lock)
  {
    AcquireSRWLockExclusive(m_lock);
  }

  ~CSRWExclusiveLock()
  {
    ReleaseSRWLockExclusive(m_lock);
  }
};

static bool FrameMetadataChanged(const D12FrameFormat& previous,
  const D12FrameFormat& current)
{
  return
    previous.hdrMetadata   != current.hdrMetadata   ||
    previous.sdrWhiteLevel != current.sdrWhiteLevel ||
    (current.hdrMetadata &&
      (memcmp(previous.displayPrimary, current.displayPrimary,
         sizeof(current.displayPrimary)) != 0 ||
       memcmp(previous.whitePoint, current.whitePoint,
         sizeof(current.whitePoint)) != 0 ||
       previous.maxDisplayLuminance       != current.maxDisplayLuminance       ||
       previous.minDisplayLuminance       != current.minDisplayLuminance       ||
       previous.maxContentLightLevel      != current.maxContentLightLevel      ||
       previous.maxFrameAverageLightLevel != current.maxFrameAverageLightLevel));
}

CSwapChainProcessor::CSwapChainProcessor(CIndirectMonitorContext * monitorContext,
    UINT64 assignmentGeneration, IDDCX_MONITOR monitor,
    CIndirectDeviceContext * devContext, IDDCX_SWAPCHAIN hSwapChain,
    LUID renderAdapter, std::shared_ptr<CD3D11Device> dx11Device,
    HANDLE newFrameEvent) :
  m_monitorContext(monitorContext),
  m_assignmentGeneration(assignmentGeneration),
  m_monitor(monitor),
  m_devContext(devContext),
  m_hSwapChain(hSwapChain),
  m_renderAdapter(renderAdapter),
  m_dx11Device(dx11Device),
  m_newFrameEvent(newFrameEvent)
{
  // Manual-reset: all worker threads wait on this, so it must stay signalled
  // once set or only one thread would ever observe termination.
  m_terminateEvent.Attach(CreateEvent(nullptr, TRUE, FALSE, nullptr));
  m_candidateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
  m_publishTimer.Attach(CreateWaitableTimerExW(nullptr, nullptr,
    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));
  if (!m_publishTimer.Get())
    m_publishTimer.Attach(CreateWaitableTimerExW(
      nullptr, nullptr, 0, TIMER_ALL_ACCESS));
  m_cursorDataEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
  m_shapeBuffer = new (std::nothrow) BYTE[512 * 512 * 4];
}

bool CSwapChainProcessor::Start()
{
  if (!m_terminateEvent.Get() || !m_candidateEvent.Get() ||
      !m_publishTimer.Get() || !m_cursorDataEvent.Get() || !m_shapeBuffer)
  {
    DEBUG_ERROR("Failed to initialize swap chain worker resources");
    return false;
  }

  // Bind the swap chain before initializing the expensive transport pipeline.
  m_thread[0].Attach(CreateThread(
    nullptr, 0, _SwapChainThread, this, 0, nullptr));
  if (!m_thread[0].Get())
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create swap chain worker");
    return false;
  }
  return true;
}

bool CSwapChainProcessor::InitializePipeline()
{
  for (;;)
  {
    if (!m_monitorContext->IsAssignmentCurrent(m_assignmentGeneration) ||
        WaitForSingleObject(m_terminateEvent.Get(), 0) == WAIT_OBJECT_0)
      return false;

    UINT64 alignSize = CPlatformInfo::GetPageSize();
    auto dx12Device = std::make_shared<CD3D12Device>(m_renderAdapter);
    const CD3D12Device::InitResult result = dx12Device->Init(
      m_devContext->GetIVSHMEM(), alignSize, !m_dx11Device->IsSoftware());
    if (result == CD3D12Device::RETRY)
    {
      const HRESULT deviceStatus =
        m_dx11Device->GetDevice()->GetDeviceRemovedReason();
      if (FAILED(deviceStatus))
      {
        DEBUG_ERROR_HR(deviceStatus,
          "D3D11 device removed during D3D12 initialization");
        return false;
      }
      continue;
    }
    if (result == CD3D12Device::FAILURE)
      return false;

    if (!m_devContext->SetupLGMP(alignSize))
    {
      DEBUG_ERROR("SetupLGMP failed");
      return false;
    }

    m_dx12Device = std::move(dx12Device);
    break;
  }

  if (!m_monitorContext->IsAssignmentCurrent(m_assignmentGeneration) ||
      WaitForSingleObject(m_terminateEvent.Get(), 0) == WAIT_OBJECT_0)
    return false;

  m_resPool.Init(m_dx11Device, m_dx12Device);
  m_fbPool.Init(this);
  const bool enableEffects = !m_dx11Device->IsSoftware();
  if (!enableEffects)
    DEBUG_INFO("Software render adapter: post-processing disabled");

  bool initialized = true;
  for (CPostProcessor& postProcessor : m_postProcessors)
    if (!postProcessor.Init(m_dx12Device, enableEffects))
    {
      initialized = false;
      break;
    }

  if (initialized)
    for (unsigned i = 1; i < ARRAYSIZE(m_postProcessors); ++i)
      if (!m_postProcessors[i].ShareEffectState(m_postProcessors[0]))
      {
        DEBUG_ERROR("Post processor effect chains do not match");
        initialized = false;
        break;
      }

  if (!initialized)
  {
    for (CPostProcessor& postProcessor : m_postProcessors)
    {
      postProcessor.Reset();
      if (!postProcessor.Init(m_dx12Device, false))
        DEBUG_ERROR("Failed to initialize post processor copy support");
    }
    DEBUG_WARN(
      "Failed to initialize post-processing effects; effects disabled");
  }

  if (!m_monitorContext->IsAssignmentCurrent(m_assignmentGeneration) ||
      WaitForSingleObject(m_terminateEvent.Get(), 0) == WAIT_OBJECT_0)
    return false;

  m_thread[2].Attach(CreateThread(
    nullptr, 0, _PublisherThread, this, 0, nullptr));
  if (!m_thread[2].Get())
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create publisher thread");
    return false;
  }
  return true;
}

CSwapChainProcessor::~CSwapChainProcessor()
{
  SetEvent(m_terminateEvent.Get());
  if (m_thread[0].Get())
    WaitForSingleObject(m_thread[0].Get(), INFINITE);
  if (m_thread[1].Get())
    WaitForSingleObject(m_thread[1].Get(), INFINITE);
  if (m_thread[2].Get())
    WaitForSingleObject(m_thread[2].Get(), INFINITE);

  // Drain in-flight GPU work / completion callbacks before releasing the
  // resources they reference. The swap chain was already released in the
  // worker epilogue, so this does not hold an IddCx frame.
  if (m_dx12Device)
  {
    m_dx12Device->WaitForIdle();
    ResetCandidates();
  }

  for (CPostProcessor& postProcessor : m_postProcessors)
    postProcessor.Reset();
  m_resPool.Reset();
  m_fbPool.Reset();
  delete[] m_shapeBuffer;
}

DWORD CALLBACK CSwapChainProcessor::_SwapChainThread(LPVOID arg)
{
  reinterpret_cast<CSwapChainProcessor*>(arg)->SwapChainThread();
  return 0;
}

static bool ArmPublishTimer(HANDLE timer, uint64_t delay)
{
  if (!timer)
    return false;

  LARGE_INTEGER due = {};
  due.QuadPart = -static_cast<LONGLONG>((delay + 99) / 100);
  if (!due.QuadPart)
    due.QuadPart = -1;
  return SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE;
}

DWORD CALLBACK CSwapChainProcessor::_PublisherThread(LPVOID arg)
{
  reinterpret_cast<CSwapChainProcessor *>(arg)->PublisherThread();
  return 0;
}

bool CSwapChainProcessor::HasReadyCandidate()
{
  bool ready = false;
  AcquireSRWLockShared(&m_candidateLock);
  for (const FrameCandidate& candidate : m_candidates)
    if (candidate.state == CANDIDATE_READY)
    {
      ready = true;
      break;
    }
  ReleaseSRWLockShared(&m_candidateLock);
  return ready;
}

void CSwapChainProcessor::PublisherThread()
{
  DWORD  avTask       = 0;
  HANDLE avTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &avTask);

  const HANDLE scheduleEvent = m_devContext->GetFrameScheduleEvent();
  HANDLE idleHandles[] =
  {
    m_terminateEvent.Get(),
    m_candidateEvent.Get(),
    scheduleEvent,
  };
  HANDLE timerHandles[] =
  {
    m_terminateEvent.Get(),
    m_candidateEvent.Get(),
    scheduleEvent,
    m_publishTimer.Get(),
  };

  for (;;)
  {
    const uint64_t            now = CFrameScheduler::Nanotime();
    uint64_t                  target;
    CFrameScheduler::Schedule schedule;
    bool                      periodic;
    bool                      republish;
    m_devContext->GetPublishTarget(
      now, target, schedule, periodic, republish);

    const bool ready = HasReadyCandidate();
    if (!ready)
    {
      m_devContext->ProcessFrameQueue();
      if (HasReadyCandidate())
        continue;

      if (republish && m_devContext->HasPublishedFrame())
      {
        if (m_devContext->RepublishFrameBuffer(schedule))
          continue;

        ArmPublishTimer(m_publishTimer.Get(), PUBLISH_RETRY_NS);
        if (WaitForMultipleObjects(
              ARRAYSIZE(timerHandles), timerHandles, FALSE, INFINITE) ==
            WAIT_OBJECT_0)
          break;
        continue;
      }

      const uint64_t replayNow = CFrameScheduler::Nanotime();
      uint64_t       replayTarget;
      if (m_devContext->GetSharedFrameTarget(replayNow, replayTarget))
      {
        bool retry = false;
        if (replayTarget <= replayNow)
        {
          if (m_devContext->ReplaySharedFrame(replayNow, retry))
            continue;
          if (!retry)
          {
            if (m_publishTimer.Get())
              CancelWaitableTimer(m_publishTimer.Get());
            if (WaitForMultipleObjects(
                  ARRAYSIZE(idleHandles), idleHandles, FALSE, INFINITE) ==
                WAIT_OBJECT_0)
              break;
            continue;
          }
        }

        const uint64_t delay = replayTarget > replayNow ?
          replayTarget - replayNow : PUBLISH_RETRY_NS;
        ArmPublishTimer(m_publishTimer.Get(), delay);
        if (WaitForMultipleObjects(
              ARRAYSIZE(timerHandles), timerHandles, FALSE, INFINITE) ==
            WAIT_OBJECT_0)
          break;
        continue;
      }

      if (m_publishTimer.Get())
        CancelWaitableTimer(m_publishTimer.Get());
      if (WaitForMultipleObjects(
            ARRAYSIZE(idleHandles), idleHandles, FALSE, INFINITE) ==
          WAIT_OBJECT_0)
        break;
      continue;
    }

    uint64_t replayTarget;
    if (m_devContext->GetSharedFrameTarget(now, replayTarget) &&
        replayTarget < target)
    {
      if (replayTarget <= now)
      {
        m_devContext->ProcessFrameQueue();
        bool retry = false;
        if (m_devContext->ReplaySharedFrame(
              CFrameScheduler::Nanotime(), retry))
          continue;

        if (retry)
          replayTarget = now + PUBLISH_RETRY_NS;
        else
          replayTarget = target;
      }

      const uint64_t delay = replayTarget > now ?
        replayTarget - now : PUBLISH_RETRY_NS;
      ArmPublishTimer(m_publishTimer.Get(), delay);
      if (WaitForMultipleObjects(
            ARRAYSIZE(timerHandles), timerHandles, FALSE, INFINITE) ==
          WAIT_OBJECT_0)
        break;
      continue;
    }

    if (target > now)
    {
      ArmPublishTimer(m_publishTimer.Get(), target - now);
      if (WaitForMultipleObjects(
            ARRAYSIZE(timerHandles), timerHandles, FALSE, INFINITE) ==
          WAIT_OBJECT_0)
        break;
      continue;
    }

    const uint64_t publishStart = CFrameScheduler::Nanotime();
    m_devContext->ProcessFrameQueue();
    if (!m_devContext->FrameBufferAvailable(schedule) ||
        !PublishNewestCandidate(
          schedule, periodic, publishStart))
    {
      ArmPublishTimer(m_publishTimer.Get(), PUBLISH_RETRY_NS);
      if (WaitForMultipleObjects(
            ARRAYSIZE(timerHandles), timerHandles, FALSE, INFINITE) ==
          WAIT_OBJECT_0)
        break;
    }
  }

  AvRevertMmThreadCharacteristics(avTaskHandle);
}

void CSwapChainProcessor::SwapChainThread()
{
  DWORD  avTask       = 0;
  HANDLE avTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &avTask);

  SwapChainThreadCore();

  // Returning success from EvtIddCxMonitorAssignSwapChain transfers ownership
  // to the driver, regardless of whether SetDevice or later initialization
  // succeeds. Release it on every worker exit.
  WdfObjectDelete((WDFOBJECT)m_hSwapChain);
  m_hSwapChain = nullptr;

  AvRevertMmThreadCharacteristics(avTaskHandle);
}

void CSwapChainProcessor::SwapChainThreadCore()
{
  ComPtr<IDXGIDevice> dxgiDevice;
  HRESULT hr = m_dx11Device->GetDevice().As(&dxgiDevice);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to get the dxgiDevice");
    return;
  }

  IDARG_IN_SWAPCHAINSETDEVICE setDevice = {};
  setDevice.pDevice = dxgiDevice.Get();

  // IddCx can unassign a swap chain before its worker binds the device. Avoid
  // using an invalidated handle; the worker epilogue still releases the
  // driver-owned swap chain.
  if (!m_monitorContext->IsAssignmentCurrent(m_assignmentGeneration) ||
      WaitForSingleObject(m_terminateEvent.Get(), 0) == WAIT_OBJECT_0)
    return;

  // A failure here (commonly DXGI_ERROR_ACCESS_LOST on the first assignment)
  // is not recoverable on this handle - IddCx reassigns a fresh swap chain,
  // which is what actually succeeds. Bail cleanly and let that happen.
  hr = IddCxSwapChainSetDevice(m_hSwapChain, &setDevice);
  if (FAILED(hr))
  {
    if (!m_monitorContext->IsAssignmentCurrent(m_assignmentGeneration) ||
        WaitForSingleObject(m_terminateEvent.Get(), 0) == WAIT_OBJECT_0)
      DEBUG_INFO("Swap chain was unassigned during device setup");
    else
      DEBUG_ERROR_HR(hr, "IddCxSwapChainSetDevice Failed");
    return;
  }
  DEBUG_INFO("Swap chain device set");

  if (IDD_IS_FUNCTION_AVAILABLE(IddCxSetRealtimeGPUPriority))
  {
    DEBUG_INFO("Using IddCxSetRealtimeGPUPriority");
    IDARG_IN_SETREALTIMEGPUPRIORITY arg = {0};
    arg.pDevice = dxgiDevice.Get();
    hr = IddCxSetRealtimeGPUPriority(m_hSwapChain, &arg);
    if (FAILED(hr))
      DEBUG_ERROR_HR(hr, "Failed to set realtime GPU thread priority");
  }
  else
  {
    DEBUG_INFO("Using SetGPUThreadPriority");
    dxgiDevice->SetGPUThreadPriority(7);
  }

  if (!InitializePipeline())
    return;

  if (!m_monitorContext->IsAssignmentCurrent(m_assignmentGeneration) ||
      WaitForSingleObject(m_terminateEvent.Get(), 0) == WAIT_OBJECT_0)
    return;

  IDARG_IN_SETUP_HWCURSOR c = {};
  c.CursorInfo.Size                  = sizeof(c.CursorInfo);
  c.CursorInfo.AlphaCursorSupport    = TRUE;
  c.CursorInfo.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL;
  c.CursorInfo.MaxX                  = 512;
  c.CursorInfo.MaxY                  = 512;
  c.hNewCursorDataAvailable          = m_cursorDataEvent.Get();
  NTSTATUS status = IddCxMonitorSetupHardwareCursor(m_monitor, &c);
  if (!NT_SUCCESS(status))
  {
    DEBUG_ERROR("IddCxMonitorSetupHardwareCursor Failed (0x%08x)", status);
    return;
  }

  m_lastShapeId = 0;
  m_thread[1].Attach(CreateThread(nullptr, 0, _CursorThread, this, 0, nullptr));

  // The replacement swap chain is fully initialized and no frame has been
  // acquired yet, so a coalesced follow-up replug may now proceed safely.
  m_devContext->OnSwapChainReady();

  // postpone sending this to ensure we dont spam messages if we end up in a
  // restart loop while waiting for a valid configuration
  g_pipe.SetGPUStatus(m_dx11Device->IsSoftware());

  UINT lastFrameNumber = 0;
  for (;;)
  {
    if (WaitForSingleObject(m_terminateEvent.Get(), 0) == WAIT_OBJECT_0)
      break;

    UINT frameNumber     = 0;
    UINT dirtyRectCount  = 0;
    UINT moveRegionCount = 0;
    ComPtr<IDXGIResource> surface;

    // The surface colour space is the source of truth for the content format.
    // Only the buffer2 acquisition path (IddCx 1.10+) reports it; on the legacy
    // path HDR is not available, so default to SDR.
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT sdrWhiteLevel = KVMFR_SDR_WHITE_LEVEL_DEFAULT;
    const uint64_t captureStart = CFrameScheduler::Nanotime();

#ifdef HAS_IDDCX_110
    if (m_devContext->HasIddCx110DDIs())
    {
      IDARG_IN_RELEASEANDACQUIREBUFFER2 acquireIn = {};
      acquireIn.Size = sizeof(acquireIn);
      acquireIn.AcquireSystemMemoryBuffer = FALSE;

      IDARG_OUT_RELEASEANDACQUIREBUFFER2 buffer = {};
      buffer.MetaData.Size = sizeof(buffer.MetaData);

      hr = IddCxSwapChainReleaseAndAcquireBuffer2(m_hSwapChain, &acquireIn, &buffer);
      if (SUCCEEDED(hr))
      {
        frameNumber    = buffer.MetaData.PresentationFrameNumber;
        dirtyRectCount = buffer.MetaData.DirtyRectCount;
        surface        = buffer.MetaData.pSurface;
        colorSpace     = buffer.MetaData.SurfaceColorSpace;
        sdrWhiteLevel  = buffer.MetaData.SdrWhiteLevel;
        m_sdrWhiteLevel.store(sdrWhiteLevel, std::memory_order_relaxed);
        UpdateHDRMetadata(buffer.MetaData);
      }
    }
    else
#endif
    {
      IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};

      hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &buffer);
      if (SUCCEEDED(hr))
      {
        frameNumber     = buffer.MetaData.PresentationFrameNumber;
        dirtyRectCount  = buffer.MetaData.DirtyRectCount;
        moveRegionCount = buffer.MetaData.MoveRegionCount;
        surface         = buffer.MetaData.pSurface;
      }
    }

    if (hr == E_PENDING)
    {
      HANDLE waitHandles[] =
      {
        m_newFrameEvent,
        m_terminateEvent.Get()
      };
      DWORD waitResult = WaitForMultipleObjects(ARRAYSIZE(waitHandles), waitHandles, FALSE, 17);
      if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT)
        continue;
      else if (waitResult == WAIT_OBJECT_0 + 1)
        break;
      else
      {
        hr = HRESULT_FROM_WIN32(waitResult);
        break;
      }
    }
    else if (SUCCEEDED(hr))
    {
      if (frameNumber != lastFrameNumber)
      {
        lastFrameNumber = frameNumber;
        if (!SwapChainNewFrame(surface, dirtyRectCount, moveRegionCount,
              colorSpace, sdrWhiteLevel, captureStart))
          DEBUG_WARN("Failed to submit frame");
      }

      // Every acquired frame must be finished before the next acquire, even if
      // its presentation number was a duplicate and no work was submitted.
      hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
      if (FAILED(hr))
      {
        // A lost path is normal (mode change/topology rebuild); Windows
        // reassigns a fresh swap chain. Just exit and let it.
        if (hr != STATUS_GRAPHICS_PATH_NOT_IN_TOPOLOGY)
          DEBUG_ERROR_HR(hr, "IddCxSwapChainFinishedProcessingFrame Failed");
        break;
      }
    }
    else
      break;
  }

}

void CSwapChainProcessor::CandidateCompletionFunction(
  CD3D12CommandSlot * slot, bool result, void * param1, void * param2)
{
  auto sc        = static_cast<CSwapChainProcessor *>(param1);
  auto candidate = static_cast<FrameCandidate *>(param2);

  uint64_t gpuStart = 0;
  uint64_t gpuEnd   = 0;
  const bool timingValid = result && slot->GetGPUTimes(gpuStart, gpuEnd);

  bool forceFrame = false;
  AcquireSRWLockExclusive(&sc->m_candidateLock);
  if (candidate->state == CANDIDATE_PREPARING)
  {
    candidate->prepareReady       = CFrameScheduler::Nanotime();
    candidate->prepareGPUStart    = gpuStart;
    candidate->prepareGPUEnd      = gpuEnd;
    candidate->prepareTimingValid = timingValid;
    candidate->state              =
      result ? CANDIDATE_READY : CANDIDATE_FREE;
    forceFrame = result && candidate->timingToken != 0;
  }
  ReleaseSRWLockExclusive(&sc->m_candidateLock);

  if (!result)
  {
    sc->SetFullPendingDamage();
    sc->m_devContext->ForceFrame();
  }
  else if (forceFrame)
    sc->m_devContext->ForceFrame();
  sc->SignalCandidateState();
}

void CSwapChainProcessor::CompletionFunction(
  CD3D12CommandSlot * slot, bool result, void * param1, void * param2)
{
  auto sc    = static_cast<CSwapChainProcessor   *>(param1);
  auto fbRes = static_cast<CFrameBufferResource *>(param2);
  const unsigned candidateIndex = fbRes->GetCandidateIndex();

  if (!result)
  {
    // The frame was reserved in LGMP before GPU submission. Make the message
    // releasable even though its contents failed.
    sc->m_devContext->FailFrameBuffer(fbRes->GetFrameIndex());
    sc->SetFullPendingDamage();
    sc->m_devContext->ForceFrame();
    sc->ReleaseCandidate(candidateIndex);
    return;
  }

  uint64_t prepareCopyStart;
  uint64_t prepareReady;
  uint64_t prepareGPUStart;
  uint64_t prepareGPUEnd;
  uint64_t timingStart;
  bool     prepareTimingValid;
  AcquireSRWLockShared(&sc->m_candidateLock);
  const FrameCandidate& candidate = sc->m_candidates[candidateIndex];
  prepareCopyStart   = candidate.prepareCopyStart;
  prepareReady       = candidate.prepareReady;
  prepareGPUStart    = candidate.prepareGPUStart;
  prepareGPUEnd      = candidate.prepareGPUEnd;
  timingStart        = candidate.timingStart;
  prepareTimingValid = candidate.prepareTimingValid;
  ReleaseSRWLockShared(&sc->m_candidateLock);

  const uint64_t publishStart = fbRes->GetCopyStart();
  uint64_t       gpuCopyStart     = 0;
  uint64_t       gpuCopyEnd       = 0;
  uint64_t       indirectCopyTime = 0;
  if (sc->m_dx12Device->IsIndirectCopy())
  {
    // GPU timestamps end at the readback copy. Track the following CPU copy
    // separately for frame metrics; benchmark wall time includes it directly.
    const uint64_t indirectCopyStart = CFrameScheduler::Nanotime();
    sc->m_devContext->WriteFrameBuffer(
      fbRes->GetFrameIndex(), fbRes->GetMap(), 0, fbRes->GetFrameSize(), false);
    indirectCopyTime = CFrameScheduler::Nanotime() - indirectCopyStart;
  }

  // Queue waits execute before the start timestamp. The end timestamp follows
  // the last copy command, separating GPU work from readiness dispatch.
  const bool gpuTimingValid =
    slot->GetGPUTimes(gpuCopyStart, gpuCopyEnd);

  // Publish readiness before sampling the endpoint. Timing has its own valid
  // flag and is published immediately afterwards.
  sc->m_devContext->FinalizeFrameBuffer(fbRes->GetFrameIndex());
  const uint64_t readyEnd = CFrameScheduler::Nanotime();

  const uint64_t postProcessStart = fbRes->GetPostProcessStart();
  uint64_t postProcessTime = prepareCopyStart - postProcessStart;
  uint64_t prepareCopyTime = prepareReady - prepareCopyStart;
  if (prepareTimingValid && prepareGPUStart >= postProcessStart &&
      prepareGPUEnd >= prepareGPUStart && prepareGPUEnd <= prepareReady)
  {
    postProcessTime = prepareGPUStart - postProcessStart;
    prepareCopyTime = prepareGPUEnd - prepareGPUStart;
  }

  uint64_t publishCopyTime = readyEnd - publishStart;
  if (gpuTimingValid && gpuCopyStart >= publishStart &&
      gpuCopyEnd >= gpuCopyStart && gpuCopyEnd <= readyEnd)
    publishCopyTime = gpuCopyEnd - gpuCopyStart + indirectCopyTime;

  const uint64_t copyTime = prepareCopyTime + publishCopyTime;
  const uint64_t elapsed  = readyEnd - postProcessStart;
  const uint64_t measured = postProcessTime + copyTime;
  const uint64_t readyTime = elapsed > measured ? elapsed - measured : 0;

  // Use matching wall-clock boundaries for both modes. The split excludes the
  // cadence hold while including the indirect CPU copy only when it occurs.
  const uint64_t timingToken = fbRes->GetTimingToken();
  if (timingToken && timingStart && prepareReady >= timingStart &&
      readyEnd >= publishStart)
  {
    const uint64_t totalTime =
      (prepareReady - timingStart) + (readyEnd - publishStart);
    sc->m_postProcessors[candidateIndex].RecordTiming(
      fbRes->GetTimingEffectIndex(), timingToken,
      fbRes->IsFullCopy(), totalTime);
  }
  sc->m_devContext->RecordFrameTiming(readyEnd - publishStart);
  sc->m_devContext->SetFrameTiming(fbRes->GetFrameIndex(),
    fbRes->GetCaptureTime(), postProcessTime, copyTime, readyTime);
  sc->m_devContext->CompleteFrameBuffer(fbRes->GetFrameIndex());
  sc->ReleaseCandidate(candidateIndex);
}


static bool IsFullDamage(const RECT * dirtyRects, unsigned nbDirtyRects,
  unsigned width, unsigned height)
{
  for (const RECT * rect = dirtyRects;
       rect < dirtyRects + nbDirtyRects; ++rect)
    if (rect->left   == 0            &&
        rect->top    == 0            &&
        rect->right  == (LONG)width  &&
        rect->bottom == (LONG)height)
      return true;

  return false;
}

static bool DirtyRectContains(const RECT& outer, const RECT& inner)
{
  return outer.left   <= inner.left  &&
         outer.top    <= inner.top   &&
         outer.right  >= inner.right &&
         outer.bottom >= inner.bottom;
}

static bool DirtyRectsTouchOrIntersect(const RECT& a, const RECT& b)
{
  return a.left <= b.right  && a.right  >= b.left &&
         a.top  <= b.bottom && a.bottom >= b.top;
}

static RECT MergeDirtyRects(const RECT& a, const RECT& b)
{
  RECT result;
  result.left   = min(a.left  , b.left  );
  result.top    = min(a.top   , b.top   );
  result.right  = max(a.right , b.right );
  result.bottom = max(a.bottom, b.bottom);
  return result;
}

static uint64_t DirtyRectArea(const RECT& rect)
{
  const uint64_t width  = (uint64_t)((int64_t)rect.right  - rect.left);
  const uint64_t height = (uint64_t)((int64_t)rect.bottom - rect.top );
  return width * height;
}

static bool AddCopyDirtyRect(RECT dirtyRects[], unsigned capacity,
  unsigned * nbDirtyRects, const RECT& dirtyRect)
{
  RECT candidate = dirtyRect;
  for (unsigned i = 0; i < *nbDirtyRects;)
  {
    if (DirtyRectContains(dirtyRects[i], candidate))
      return true;

    const RECT merged = MergeDirtyRects(dirtyRects[i], candidate);
    // Reduce command and overlap cost without copying more pixels than the
    // two original rectangles would have copied.
    if (DirtyRectContains(candidate, dirtyRects[i]) ||
        (DirtyRectsTouchOrIntersect(dirtyRects[i], candidate) &&
          DirtyRectArea(merged) <=
            DirtyRectArea(dirtyRects[i]) + DirtyRectArea(candidate)))
    {
      candidate = merged;
      --(*nbDirtyRects);
      dirtyRects[i] = dirtyRects[*nbDirtyRects];
      i = 0;
      continue;
    }

    ++i;
  }

  if (*nbDirtyRects >= capacity)
    return false;

  dirtyRects[(*nbDirtyRects)++] = candidate;
  return true;
}

static bool CopyAreaCoversFrame(const RECT * dirtyRects,
  unsigned nbDirtyRects, unsigned width, unsigned height)
{
  const uint64_t frameArea = (uint64_t)width * height;
  uint64_t       copyArea  = 0;

  for (const RECT * rect = dirtyRects;
       rect < dirtyRects + nbDirtyRects; ++rect)
  {
    const uint64_t area = DirtyRectArea(*rect);
    if (area >= frameArea - copyArea)
      return true;
    copyArea += area;
  }

  return false;
}

static bool ClipDirtyRect(RECT& rect, unsigned width, unsigned height)
{
  const LONG maxRight  = (LONG)width;
  const LONG maxBottom = (LONG)height;

  if (rect.left   < 0        ) rect.left   = 0;
  if (rect.top    < 0        ) rect.top    = 0;
  if (rect.right  > maxRight ) rect.right  = maxRight;
  if (rect.bottom > maxBottom) rect.bottom = maxBottom;

  return rect.left < rect.right && rect.top < rect.bottom;
}

static void ClipDirtyRects(RECT dirtyRects[], unsigned * nbDirtyRects,
  unsigned width, unsigned height)
{
  unsigned out = 0;
  for (unsigned i = 0; i < *nbDirtyRects; ++i)
  {
    RECT rect = dirtyRects[i];
    if (ClipDirtyRect(rect, width, height))
      dirtyRects[out++] = rect;
  }
  *nbDirtyRects = out;
}

static FrameType GetFrameType(DXGI_FORMAT format)
{
  switch (format)
  {
    case DXGI_FORMAT_B8G8R8A8_UNORM    : return FRAME_TYPE_BGRA;
    case DXGI_FORMAT_R8G8B8A8_UNORM    : return FRAME_TYPE_RGBA;
    case DXGI_FORMAT_R10G10B10A2_UNORM : return FRAME_TYPE_RGBA10;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return FRAME_TYPE_RGBA16F;
    default                            : return FRAME_TYPE_INVALID;
  }
}

static void AccumulatePendingDamage(
  RECT pendingDirtyRects[], unsigned * nbPendingDirtyRects,
  bool * hasPendingDamage, const RECT dirtyRects[], unsigned nbDirtyRects)
{
  if (nbDirtyRects > LG_MAX_DIRTY_RECTS)
    nbDirtyRects = 0;

  if (!*hasPendingDamage)
  {
    *hasPendingDamage    = true;
    *nbPendingDirtyRects = nbDirtyRects;
    if (nbDirtyRects)
      memcpy(pendingDirtyRects, dirtyRects,
        nbDirtyRects * sizeof(*pendingDirtyRects));
    return;
  }

  // Zero dirty rectangles represents full-frame damage. Once an accumulated
  // set is full, no later rectangles can narrow that same set again.
  if (*nbPendingDirtyRects == 0 || nbDirtyRects == 0 ||
      *nbPendingDirtyRects + nbDirtyRects > LG_MAX_DIRTY_RECTS)
  {
    *nbPendingDirtyRects = 0;
    return;
  }

  memcpy(pendingDirtyRects + *nbPendingDirtyRects, dirtyRects,
    nbDirtyRects * sizeof(*pendingDirtyRects));
  *nbPendingDirtyRects += nbDirtyRects;
}

void CSwapChainProcessor::SetFullPendingDamage()
{
  AcquireSRWLockExclusive(&m_damageLock);
  m_hasPendingDamage    = true;
  m_nbPendingDirtyRects = 0;
  for (CandidateDamageTail& tail : m_candidateDamageTail)
    if (tail.active)
    {
      tail.hasDamage    = true;
      tail.nbDirtyRects = 0;
    }
  ReleaseSRWLockExclusive(&m_damageLock);
}

void CSwapChainProcessor::AccumulateFrameDamage(
  const RECT * dirtyRects, unsigned nbDirtyRects)
{
  AcquireSRWLockExclusive(&m_damageLock);
  AccumulatePendingDamage(
    m_pendingDirtyRects, &m_nbPendingDirtyRects, &m_hasPendingDamage,
    dirtyRects, nbDirtyRects);
  for (CandidateDamageTail& tail : m_candidateDamageTail)
    if (tail.active)
      AccumulatePendingDamage(
        tail.dirtyRects, &tail.nbDirtyRects, &tail.hasDamage,
        dirtyRects, nbDirtyRects);
  ReleaseSRWLockExclusive(&m_damageLock);
}

int CSwapChainProcessor::AcquireCandidate(bool exclusiveSample)
{
  int      selected   = -1;
  uint64_t oldest     = UINT64_MAX;
  bool     superseded = false;
  bool     idle       = true;

  AcquireSRWLockExclusive(&m_candidateLock);
  for (unsigned i = 0; i < ARRAYSIZE(m_candidates); ++i)
    if (m_candidates[i].state != CANDIDATE_FREE)
      idle = false;
    else if (selected < 0)
      selected = static_cast<int>(i);

  // Effect timing samples must not queue behind work which can later be
  // superseded, otherwise that discarded work contaminates the sample.
  if (exclusiveSample && !idle)
    selected = -1;

  unsigned readyCount = 0;
  for (const FrameCandidate& candidate : m_candidates)
    if (candidate.state == CANDIDATE_READY)
      ++readyCount;

  if (!exclusiveSample && selected < 0 && readyCount > 1)
    for (unsigned i = 0; i < ARRAYSIZE(m_candidates); ++i)
      if (m_candidates[i].state == CANDIDATE_READY &&
          m_candidates[i].sequence < oldest)
      {
        selected = static_cast<int>(i);
        oldest   = m_candidates[i].sequence;
      }

  if (selected >= 0)
  {
    FrameCandidate& candidate =
      m_candidates[static_cast<unsigned>(selected)];
    superseded         = candidate.state == CANDIDATE_READY;
    candidate.state    = CANDIDATE_PREPARING;
    candidate.sequence = ++m_candidateSequence;
  }
  ReleaseSRWLockExclusive(&m_candidateLock);

  if (superseded)
    m_devContext->FrameSuperseded();
  return selected;
}

void CSwapChainProcessor::ReleaseCandidate(unsigned candidateIndex)
{
  if (candidateIndex >= ARRAYSIZE(m_candidates))
    return;

  AcquireSRWLockExclusive(&m_candidateLock);
  m_candidates[candidateIndex].state = CANDIDATE_FREE;
  ReleaseSRWLockExclusive(&m_candidateLock);
  SignalCandidateState();
}

static bool ResourceDescMatches(
  const D3D12_RESOURCE_DESC& left, const D3D12_RESOURCE_DESC& right)
{
  // Alignment is allocation metadata. GetDesc may report the resolved value
  // when the creation descriptor requested automatic alignment.
  return
    left.Dimension          == right.Dimension          &&
    left.Width              == right.Width              &&
    left.Height             == right.Height             &&
    left.DepthOrArraySize   == right.DepthOrArraySize   &&
    left.MipLevels          == right.MipLevels          &&
    left.Format             == right.Format             &&
    left.SampleDesc.Count   == right.SampleDesc.Count   &&
    left.SampleDesc.Quality == right.SampleDesc.Quality &&
    left.Layout             == right.Layout             &&
    left.Flags              == right.Flags;
}

bool CSwapChainProcessor::EnsureCandidateResource(
  unsigned candidateIndex, size_t frameSize)
{
  FrameCandidate& candidate = m_candidates[candidateIndex];

  // Keep the transport layout in local GPU memory so publication does not
  // combine texture detiling with the IVSHMEM or readback transfer.
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width              = frameSize;
  desc.Height             = 1;
  desc.DepthOrArraySize   = 1;
  desc.MipLevels          = 1;
  desc.Format             = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count   = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags              = D3D12_RESOURCE_FLAG_NONE;

  if (candidate.resource &&
      ResourceDescMatches(candidate.resource->GetDesc(), desc))
    return true;

  candidate.resource.Reset();

  D3D12_HEAP_PROPERTIES heapProps = {};
  heapProps.Type                 = D3D12_HEAP_TYPE_DEFAULT;
  heapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  heapProps.CreationNodeMask     = 1;
  heapProps.VisibleNodeMask      = 1;

  const HRESULT hr = m_dx12Device->GetDevice()->CreateCommittedResource(
    &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
    nullptr, IID_PPV_ARGS(&candidate.resource));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create retained frame candidate");
    return false;
  }

  static const WCHAR * names[] =
  {
    L"Frame Candidate 0",
    L"Frame Candidate 1",
  };
  candidate.resource->SetName(names[candidateIndex]);
  return true;
}

void CSwapChainProcessor::ResetCandidates()
{
  AcquireSRWLockExclusive(&m_candidateLock);
  for (FrameCandidate& candidate : m_candidates)
    candidate = {};
  ReleaseSRWLockExclusive(&m_candidateLock);

  AcquireSRWLockExclusive(&m_damageLock);
  for (CandidateDamageTail& tail : m_candidateDamageTail)
    tail = {};
  ReleaseSRWLockExclusive(&m_damageLock);
  SignalCandidateState();
}

void CSwapChainProcessor::SignalCandidateState()
{
  SetEvent(m_candidateEvent.Get());
}

bool CSwapChainProcessor::PublishNewestCandidate(
  const CFrameScheduler::Schedule& schedule, bool periodic,
  uint64_t publishStart)
{
  int      selectedCandidate = -1;
  uint64_t newestSequence    = 0;

  AcquireSRWLockExclusive(&m_candidateLock);
  for (unsigned i = 0; i < ARRAYSIZE(m_candidates); ++i)
    if (m_candidates[i].state == CANDIDATE_READY &&
        (selectedCandidate < 0 ||
          m_candidates[i].sequence > newestSequence))
    {
      selectedCandidate = static_cast<int>(i);
      newestSequence    = m_candidates[i].sequence;
    }

  if (selectedCandidate >= 0)
    for (unsigned i = 0; i < ARRAYSIZE(m_candidates); ++i)
      if (static_cast<int>(i) == selectedCandidate)
        m_candidates[i].state = CANDIDATE_PUBLISHING;
      else if (m_candidates[i].state == CANDIDATE_READY)
        m_candidates[i].state = CANDIDATE_HELD;
  ReleaseSRWLockExclusive(&m_candidateLock);

  if (selectedCandidate < 0)
    return false;
  const unsigned candidateIndex =
    static_cast<unsigned>(selectedCandidate);

  const auto restoreCandidates = [this, candidateIndex]()
  {
    AcquireSRWLockExclusive(&m_candidateLock);
    if (m_candidates[candidateIndex].state == CANDIDATE_PUBLISHING)
      m_candidates[candidateIndex].state = CANDIDATE_READY;
    for (FrameCandidate& candidate : m_candidates)
      if (candidate.state == CANDIDATE_HELD)
        candidate.state = CANDIDATE_READY;
    ReleaseSRWLockExclusive(&m_candidateLock);
    SignalCandidateState();
  };

  CSRWExclusiveLock pipelineLock(&m_pipelineLock);
  AcquireSRWLockShared(&m_candidateLock);
  const bool candidateValid =
    m_candidates[candidateIndex].state == CANDIDATE_PUBLISHING &&
    m_candidates[candidateIndex].resource.Get();
  ReleaseSRWLockShared(&m_candidateLock);
  if (!candidateValid)
  {
    restoreCandidates();
    return false;
  }

  FrameCandidate& candidate        = m_candidates[candidateIndex];
  CPostProcessor& postProcessor    = m_postProcessors[candidateIndex];
  const uint64_t candidateSequence = candidate.sequence;

  auto buffer = m_devContext->PrepareFrameBuffer(
    candidate.pitch,
    candidate.srcFormat,
    candidate.dstFormat,
    candidate.dirtyRects,
    candidate.nbDirtyRects);
  if (!buffer.mem)
  {
    restoreCandidates();
    return false;
  }

  CFrameBufferResource * fbRes =
    m_fbPool.Get(buffer, candidate.frameSize);
  if (!fbRes)
  {
    m_devContext->AbortFrameBuffer(buffer.frameIndex);
    restoreCandidates();
    DEBUG_ERROR("Failed to get a CFrameBufferResource from the pool");
    SetFullPendingDamage();
    return false;
  }

  CD3D12CommandSlot * copySlot =
    m_dx12Device->GetCopySlot(candidateIndex);
  if (!copySlot)
  {
    m_devContext->AbortFrameBuffer(buffer.frameIndex);
    restoreCandidates();
    DEBUG_ERROR("Failed to get a copy CommandSlot for publication");
    SetFullPendingDamage();
    return false;
  }

  RECT     previousDirtyRects[LG_MAX_DIRTY_RECTS] = {};
  unsigned nbPreviousDirtyRects                   = 0;
  AcquireSRWLockShared(&m_damageLock);
  nbPreviousDirtyRects = m_nbDirtyRects;
  if (nbPreviousDirtyRects)
    memcpy(previousDirtyRects, m_dirtyRects,
      nbPreviousDirtyRects * sizeof(*previousDirtyRects));
  ReleaseSRWLockShared(&m_damageLock);

  RECT     copyDirtyRects[LG_MAX_DIRTY_RECTS * 2] = {};
  unsigned nbCopyDirtyRects                       = 0;
  bool     fullCopy = buffer.fullCopy ||
    candidate.nbDirtyRects == 0 || nbPreviousDirtyRects == 0;

  if (!fullCopy)
  {
    for (const RECT * rect = previousDirtyRects;
         rect < previousDirtyRects + nbPreviousDirtyRects && !fullCopy;
         ++rect)
    {
      RECT clipped = *rect;
      if (ClipDirtyRect(clipped,
            candidate.dstFormat.width, candidate.dstFormat.height) &&
          !AddCopyDirtyRect(copyDirtyRects, ARRAYSIZE(copyDirtyRects),
            &nbCopyDirtyRects, clipped))
        fullCopy = true;
    }

    for (const RECT * rect = candidate.dirtyRects;
         rect < candidate.dirtyRects + candidate.nbDirtyRects && !fullCopy;
         ++rect)
      if (!AddCopyDirtyRect(copyDirtyRects, ARRAYSIZE(copyDirtyRects),
          &nbCopyDirtyRects, *rect))
        fullCopy = true;

    if (!fullCopy)
      fullCopy = IsFullDamage(
          copyDirtyRects, nbCopyDirtyRects,
          candidate.dstFormat.width, candidate.dstFormat.height) ||
        CopyAreaCoversFrame(
          copyDirtyRects, nbCopyDirtyRects,
          candidate.dstFormat.width, candidate.dstFormat.height);

    if (!fullCopy)
      fullCopy = postProcessor.ShouldCopyFully(
        copyDirtyRects, nbCopyDirtyRects);
  }

  fbRes->SetTiming(
    candidate.captureTime, candidate.postProcessStart, publishStart);
  fbRes->SetCandidateIndex(candidateIndex);
  fbRes->SetPostProcessSample(
    candidate.timingEffectIndex, candidate.timingToken, fullCopy);
  copySlot->SetCompletionCallback(&CompletionFunction, this, fbRes);

  copySlot->BeginTiming();
  postProcessor.CopyFromCandidate(
    copySlot->GetGfxList(), fbRes->Get().Get(), candidate.resource.Get(),
    copyDirtyRects, nbCopyDirtyRects, fullCopy);
  copySlot->EndTiming();

  // Reserve the LGMP message before submitting the copy. This makes post
  // failure recoverable without racing a very fast GPU completion callback.
  if (!m_devContext->PublishFrameBuffer(
        buffer.frameIndex, schedule))
  {
    copySlot->Cancel();
    m_devContext->AbortFrameBuffer(buffer.frameIndex);
    restoreCandidates();
    return false;
  }

  if (!copySlot->Execute())
  {
    AcquireSRWLockShared(&m_candidateLock);
    const bool callbackPending =
      candidate.state == CANDIDATE_PUBLISHING;
    ReleaseSRWLockShared(&m_candidateLock);
    if (callbackPending && !copySlot->HasSubmittedWork())
    {
      m_devContext->FailFrameBuffer(buffer.frameIndex);
      SetFullPendingDamage();
      ReleaseCandidate(candidateIndex);
    }
    m_devContext->ForceFrame();

    AcquireSRWLockExclusive(&m_candidateLock);
    for (FrameCandidate& held : m_candidates)
      if (held.state == CANDIDATE_HELD)
        held.state = CANDIDATE_READY;
    ReleaseSRWLockExclusive(&m_candidateLock);
    SignalCandidateState();
    return false;
  }

  AcquireSRWLockExclusive(&m_damageLock);
  if (candidate.nbDirtyRects)
    memcpy(m_dirtyRects, candidate.dirtyRects,
      candidate.nbDirtyRects * sizeof(*m_dirtyRects));
  m_nbDirtyRects = candidate.nbDirtyRects;
  CandidateDamageTail& tail = m_candidateDamageTail[candidateIndex];
  if (tail.active && tail.ownerSequence == candidateSequence)
  {
    m_hasPendingDamage    = tail.hasDamage;
    m_nbPendingDirtyRects = tail.nbDirtyRects;
    if (tail.hasDamage && tail.nbDirtyRects)
      memcpy(m_pendingDirtyRects, tail.dirtyRects,
        tail.nbDirtyRects * sizeof(*m_pendingDirtyRects));
    tail.ownerSequence = 0;
    tail.active        = false;
  }
  ReleaseSRWLockExclusive(&m_damageLock);

  m_devContext->CommitFrameBuffer(
    buffer.frameIndex, schedule, periodic);

  unsigned superseded = 0;
  AcquireSRWLockExclusive(&m_candidateLock);
  for (FrameCandidate& held : m_candidates)
    if (held.state == CANDIDATE_HELD)
    {
      held.state = CANDIDATE_FREE;
      ++superseded;
    }
  ReleaseSRWLockExclusive(&m_candidateLock);
  for (unsigned i = 0; i < superseded; ++i)
    m_devContext->FrameSuperseded();
  SignalCandidateState();
  return true;
}

#ifdef HAS_IDDCX_110
void CSwapChainProcessor::UpdateHDRMetadata(const IDDCX_METADATA2& metadata)
{
  if (!(metadata.ValidFlags & IDDCX_METADATA2_VALID_FLAGS_HDR10METADATA))
    return;

  const IDDCX_HDR10_FRAME_METADATA& frame = metadata.Hdr10FrameMetaData;
  switch (frame.Type)
  {
    case IDDCX_HDR10_FRAME_METADATA_TYPE_DEFAULT:
      if (!m_useDefaultHDRMetadata)
        DEBUG_TRACE("HDR10 frame metadata switched to the monitor default");
      m_useDefaultHDRMetadata = true;
      m_hasNewHDRMetadata     = false;
      break;

    case IDDCX_HDR10_FRAME_METADATA_TYPE_UNCHANGED:
      break;

    case IDDCX_HDR10_FRAME_METADATA_TYPE_NEW:
      if (!m_hasNewHDRMetadata ||
          memcmp(&m_newHDRMetadata, &frame.NewMetaData,
            sizeof(m_newHDRMetadata)) != 0)
        DEBUG_TRACE("Received new HDR10 frame metadata");
      m_newHDRMetadata        = frame.NewMetaData;
      m_useDefaultHDRMetadata = false;
      m_hasNewHDRMetadata     = true;
      break;

    default:
      DEBUG_WARN("Invalid HDR10 frame metadata type %u",
        static_cast<unsigned>(frame.Type));
      break;
  }
}
#endif

bool CSwapChainProcessor::GetContentHDRMetadata(D12FrameFormat& format) const
{
#ifdef HAS_IDDCX_110
  // The monitor default describes the virtual display, not the content. Only
  // publish an explicit per-frame metadata block to downstream consumers.
  if (m_useDefaultHDRMetadata || !m_hasNewHDRMetadata)
    return false;

  const IDDCX_HDR10_METADATA& metadata = m_newHDRMetadata;
  format.displayPrimary[0][0]      = metadata.RedPrimary  [0];
  format.displayPrimary[0][1]      = metadata.RedPrimary  [1];
  format.displayPrimary[1][0]      = metadata.GreenPrimary[0];
  format.displayPrimary[1][1]      = metadata.GreenPrimary[1];
  format.displayPrimary[2][0]      = metadata.BluePrimary [0];
  format.displayPrimary[2][1]      = metadata.BluePrimary [1];
  format.whitePoint    [0]         = metadata.WhitePoint  [0];
  format.whitePoint    [1]         = metadata.WhitePoint  [1];
  format.maxDisplayLuminance       = metadata.MaxMasteringLuminance;
  format.minDisplayLuminance       = metadata.MinMasteringLuminance;
  format.maxContentLightLevel      = metadata.MaxContentLightLevel;
  format.maxFrameAverageLightLevel = metadata.MaxFrameAverageLightLevel;
  return true;
#else
  UNREFERENCED_PARAMETER(format);
  return false;
#endif
}

bool CSwapChainProcessor::SwapChainNewFrame(ComPtr<IDXGIResource> acquiredBuffer,
  unsigned dirtyRectCount, unsigned moveRegionCount,
  DXGI_COLOR_SPACE_TYPE colorSpace, UINT sdrWhiteLevel,
  uint64_t captureStart)
{
  const uint64_t postProcessStart = CFrameScheduler::Nanotime();
  const uint64_t captureTime      = postProcessStart - captureStart;

  ComPtr<ID3D11Texture2D> texture;
  HRESULT hr = acquiredBuffer.As(&texture);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to obtain the ID3D11Texture2D from the acquiredBuffer");
    SetFullPendingDamage();
    return false;
  }

  CInteropResource * srcRes = m_resPool.Get(texture);
  if (!srcRes)
  {
    DEBUG_ERROR("Failed to get a CInteropResource from the pool");
    SetFullPendingDamage();
    return false;
  }

  RECT dirtyRects[LG_MAX_DIRTY_RECTS] = {0};
  bool noImageUpdate = false;
  if (moveRegionCount || dirtyRectCount > ARRAYSIZE(dirtyRects))
  {
    // Move regions are not represented by the dirty rectangle list. Copy the
    // full surface so the alternating destinations remain coherent.
    srcRes->SetFullDamage();
  }
  else
  {
    IDARG_IN_GETDIRTYRECTS dirtyIn = {};
    dirtyIn.DirtyRectInCount = dirtyRectCount;
    dirtyIn.pDirtyRects      = dirtyRects;

    IDARG_OUT_GETDIRTYRECTS dirtyOut = {};
    hr = IddCxSwapChainGetDirtyRects(m_hSwapChain, &dirtyIn, &dirtyOut);
    if (FAILED(hr))
    {
      DEBUG_ERROR_HR(hr, "IddCxSwapChainGetDirtyRects Failed");
      srcRes->SetFullDamage();
    }
    else if (dirtyOut.DirtyRectOutCount == 1 &&
        dirtyRects[0].left   == 0 && dirtyRects[0].top    == 0 &&
        dirtyRects[0].right  == 0 && dirtyRects[0].bottom == 0)
    {
      // One empty rectangle is IddCx's static-desktop re-encode marker. It
      // does not describe an image update and must not become full damage.
      noImageUpdate = true;
      srcRes->SetDirtyRects(nullptr, 0);
    }
    else
      srcRes->SetDirtyRects(dirtyRects, dirtyOut.DirtyRectOutCount);
  }

  D3D12_RESOURCE_DESC srcDesc = srcRes->GetRes()->GetDesc();
  if (!noImageUpdate)
  {
    m_devContext->ObserveFrame(postProcessStart);
    AccumulateFrameDamage(
      srcRes->GetDirtyRects(), srcRes->GetDirtyRectCount());
  }

  D12FrameFormat srcFormat = {};
  srcFormat.desc           = srcDesc;
  srcFormat.width          = (unsigned)srcDesc.Width;
  srcFormat.height         = srcDesc.Height;
  srcFormat.format         = GetFrameType(srcDesc.Format);
  srcFormat.sdrWhiteLevel  = sdrWhiteLevel;
  srcFormat.colorTransform = m_devContext->GetColorTransform();

  switch (colorSpace)
  {
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
      // HDR10: BT.2020 primaries with the PQ (ST.2084) transfer function
      // already applied to the pixel data.
      srcFormat.hdr   = true;
      srcFormat.hdrPQ = true;
      if (!GetContentHDRMetadata(srcFormat))
      {
        // No per-content metadata is active. The pixels are still PQ-encoded,
        // so keep the PQ flag and use BT.2020/PQ defaults internally rather
        // than publishing the virtual monitor metadata as content metadata.
        // BT.2020 primaries (in 0.00002 units):
        srcFormat.displayPrimary[0][0] = 35400; // Rx
        srcFormat.displayPrimary[0][1] = 14600; // Ry
        srcFormat.displayPrimary[1][0] =  8500; // Gx
        srcFormat.displayPrimary[1][1] = 39850; // Gy
        srcFormat.displayPrimary[2][0] =  6550; // Bx
        srcFormat.displayPrimary[2][1] =  2300; // By
        // D65 white point (in 0.00002 units):
        srcFormat.whitePoint[0] = 15635;
        srcFormat.whitePoint[1] = 16450;
        // Cover the complete PQ signal range.
        srcFormat.maxDisplayLuminance = HDR_PQ_MAX_LUMINANCE;
        srcFormat.minDisplayLuminance = HDR_PQ_MIN_LUMINANCE;
        // Content light levels unknown:
        srcFormat.maxContentLightLevel      = 0;
        srcFormat.maxFrameAverageLightLevel = 0;
      }
      else
        srcFormat.hdrMetadata = true;
      break;

    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
      // scRGB: linear (FP16) content with BT.709 primaries. HDR, but the PQ
      // curve has not been applied.
      srcFormat.hdr   = true;
      srcFormat.hdrPQ = false;
      if (!GetContentHDRMetadata(srcFormat))
      {
        // No per-content metadata is active. Use reasonable internal defaults
        // without publishing the virtual monitor metadata downstream.
        // BT.709/sRGB primaries (in 0.00002 units):
        srcFormat.displayPrimary[0][0] = 32000; // Rx
        srcFormat.displayPrimary[0][1] = 16500; // Ry
        srcFormat.displayPrimary[1][0] = 15000; // Gx
        srcFormat.displayPrimary[1][1] = 30000; // Gy
        srcFormat.displayPrimary[2][0] =  7500; // Bx
        srcFormat.displayPrimary[2][1] =  3000; // By
        // D65 white point (in 0.00002 units):
        srcFormat.whitePoint[0] = 15635;
        srcFormat.whitePoint[1] = 16450;
        // Mastering luminances follow SMPTE ST 2086 units: max in whole cd/m²,
        // min in 0.0001 cd/m². 80 cd/m² display, 0.005 cd/m² black:
        srcFormat.maxDisplayLuminance = 80;
        srcFormat.minDisplayLuminance = 50;
        // Content light levels unknown:
        srcFormat.maxContentLightLevel      = 0;
        srcFormat.maxFrameAverageLightLevel = 0;
      }
      else
        srcFormat.hdrMetadata = true;
      break;

    default:
      // Everything else (e.g. RGB_FULL_G22_NONE_P709) is SDR.
      srcFormat.hdr   = false;
      srcFormat.hdrPQ = false;
      break;
  }

  bool frameMetadataChanged     = false;
  bool needsReconfigure         = false;
  bool postProcessFormatChanged = false;
  bool requiresFullDamage       = false;
  unsigned timingEffectIndex    = 0;
  uint64_t timingToken          = 0;
  {
    CSRWExclusiveLock pipelineLock(&m_pipelineLock);
    m_postProcessors[0].Update(srcFormat);

    frameMetadataChanged = noImageUpdate &&
      FrameMetadataChanged(
        m_postProcessors[0].GetOutputFormat(), srcFormat);

    for (const CPostProcessor& postProcessor : m_postProcessors)
      if (postProcessor.NeedsReconfigure(srcFormat))
      {
        needsReconfigure = true;
        break;
      }

    // A format change can replace resources referenced by either retained
    // candidate. Stop publication, drain both queues, then invalidate them.
    if (needsReconfigure)
    {
      AcquireSRWLockExclusive(&m_damageLock);
      m_nbDirtyRects = 0;
      ReleaseSRWLockExclusive(&m_damageLock);
      SetFullPendingDamage();
      m_dx12Device->WaitForIdle();
      ResetCandidates();
    }

    bool configurationStable = false;
    for (unsigned pass = 0; pass < 2 && !configurationStable; ++pass)
    {
      for (unsigned i = 0; i < ARRAYSIZE(m_postProcessors); ++i)
      {
        bool formatChanged = false;
        if (!m_postProcessors[i].Configure(srcFormat, &formatChanged))
        {
          SetFullPendingDamage();
          return false;
        }

        if (i == 0)
          postProcessFormatChanged |= formatChanged;
      }

      configurationStable = true;
      for (const CPostProcessor& postProcessor : m_postProcessors)
        if (postProcessor.NeedsReconfigure(srcFormat))
        {
          configurationStable = false;
          break;
        }
    }

    if (!configurationStable)
    {
      DEBUG_ERROR("Post processor configuration did not stabilize");
      SetFullPendingDamage();
      return false;
    }

    if (postProcessFormatChanged)
    {
      AcquireSRWLockExclusive(&m_damageLock);
      m_nbDirtyRects = 0;
      ReleaseSRWLockExclusive(&m_damageLock);
      SetFullPendingDamage();
    }
    else if (frameMetadataChanged)
      SetFullPendingDamage();

    requiresFullDamage = m_postProcessors[0].RequiresFullDamage();
    if (requiresFullDamage)
      SetFullPendingDamage();

    m_postProcessors[0].GetTimingToken(
      &timingEffectIndex, &timingToken);
  }

  if (needsReconfigure || postProcessFormatChanged || frameMetadataChanged)
    m_devContext->ForceFrame();

  if (noImageUpdate)
  {
    AcquireSRWLockShared(&m_damageLock);
    const bool hasPendingDamage = m_hasPendingDamage;
    ReleaseSRWLockShared(&m_damageLock);
    if (!hasPendingDamage)
      return true;
  }

  const int selectedCandidate = AcquireCandidate(timingToken != 0);
  if (selectedCandidate < 0)
  {
    m_devContext->FrameSuperseded();
    return true;
  }
  const unsigned candidateIndex =
    static_cast<unsigned>(selectedCandidate);
  FrameCandidate& candidate = m_candidates[candidateIndex];

  CSRWExclusiveLock pipelineLock(&m_pipelineLock);
  CPostProcessor& postProcessor = m_postProcessors[candidateIndex];
  const D12FrameFormat& dstFormat = postProcessor.GetOutputFormat();

  RECT     currentDirtyRects[LG_MAX_DIRTY_RECTS] = {};
  unsigned nbDirtyRects                          = 0;
  AcquireSRWLockExclusive(&m_damageLock);
  if (m_hasPendingDamage)
  {
    nbDirtyRects = m_nbPendingDirtyRects;
    if (nbDirtyRects)
      memcpy(currentDirtyRects, m_pendingDirtyRects,
        nbDirtyRects * sizeof(*currentDirtyRects));
  }
  CandidateDamageTail& tail = m_candidateDamageTail[candidateIndex];
  tail.ownerSequence = candidate.sequence;
  tail.nbDirtyRects  = 0;
  tail.hasDamage     = false;
  tail.active        = true;
  ReleaseSRWLockExclusive(&m_damageLock);

  CD3D12CommandSlot * copySlot =
    m_dx12Device->GetCopySlot(candidateIndex);
  if (!copySlot)
  {
    ReleaseCandidate(candidateIndex);
    DEBUG_ERROR("Failed to get a copy CommandSlot");
    SetFullPendingDamage();
    return false;
  }
  // Candidate and copy-slot acquisition are common to both benchmark modes.
  const uint64_t timingStart = timingToken ?
    CFrameScheduler::Nanotime() : 0;

  ComPtr<ID3D12Resource> copySrcResource = srcRes->GetRes();
  CD3D12CommandSlot    * computeSlot     = nullptr;
  if (postProcessor.HasActiveEffects())
  {
    computeSlot = m_dx12Device->GetComputeSlot(candidateIndex);
    if (!computeSlot)
    {
      copySlot->Cancel();
      ReleaseCandidate(candidateIndex);
      DEBUG_ERROR("Failed to get a compute CommandSlot");
      SetFullPendingDamage();
      return false;
    }
  }

  /**
   * Even though we have not performed any copy/draw operations we still need
   * to use a fence. Because we share this texture with DirectX12 it is able to
   * read from it before IddCx has finished updating it.
   */
  if (!srcRes->Signal())
  {
    if (computeSlot)
      computeSlot->Cancel();
    copySlot->Cancel();
    ReleaseCandidate(candidateIndex);
    SetFullPendingDamage();
    return false;
  }

  if (computeSlot)
  {
    if (!srcRes->Sync(*computeSlot))
    {
      computeSlot->Cancel();
      copySlot->Cancel();
      ReleaseCandidate(candidateIndex);
      SetFullPendingDamage();
      return false;
    }

    copySrcResource = postProcessor.Run(
      computeSlot->GetGfxList(), copySrcResource,
      currentDirtyRects, &nbDirtyRects);
    if (!copySrcResource)
    {
      computeSlot->Cancel();
      copySlot->Cancel();
      ReleaseCandidate(candidateIndex);
      DEBUG_ERROR("Post processor returned no output resource");
      SetFullPendingDamage();
      return false;
    }

    if (!computeSlot->Execute())
    {
      copySlot->Cancel();
      m_dx12Device->WaitForIdle();
      ReleaseCandidate(candidateIndex);
      SetFullPendingDamage();
      return false;
    }

    if (!copySlot->WaitFor(*computeSlot))
    {
      copySlot->Cancel();
      m_dx12Device->WaitForIdle();
      ReleaseCandidate(candidateIndex);
      DEBUG_ERROR("Failed to queue compute synchronization");
      SetFullPendingDamage();
      return false;
    }
  }
  else if (!srcRes->Sync(*copySlot))
  {
    copySlot->Cancel();
    ReleaseCandidate(candidateIndex);
    DEBUG_ERROR("Failed to queue source synchronization");
    SetFullPendingDamage();
    return false;
  }

  ClipDirtyRects(currentDirtyRects, &nbDirtyRects,
    dstFormat.width, dstFormat.height);

  const size_t frameSize = postProcessor.GetOutputSize();
  if (!EnsureCandidateResource(candidateIndex, frameSize))
  {
    copySlot->Cancel();
    if (computeSlot)
      m_dx12Device->WaitForIdle();
    ReleaseCandidate(candidateIndex);
    SetFullPendingDamage();
    return false;
  }

  candidate.srcFormat          = srcFormat;
  candidate.dstFormat          = dstFormat;
  candidate.nbDirtyRects       = nbDirtyRects;
  candidate.pitch              = postProcessor.GetOutputPitch();
  candidate.frameSize          = frameSize;
  candidate.captureTime        = captureTime;
  candidate.postProcessStart   = postProcessStart;
  candidate.prepareCopyStart   = CFrameScheduler::Nanotime();
  candidate.prepareReady       = 0;
  candidate.prepareGPUStart    = 0;
  candidate.prepareGPUEnd      = 0;
  candidate.timingStart        = timingStart;
  candidate.prepareTimingValid = false;
  if (nbDirtyRects)
    memcpy(candidate.dirtyRects, currentDirtyRects,
      nbDirtyRects * sizeof(*candidate.dirtyRects));
  candidate.timingEffectIndex  = timingEffectIndex;
  candidate.timingToken        = timingToken;

  copySlot->SetCompletionCallback(
    &CandidateCompletionFunction, this, &candidate);
  copySlot->BeginTiming();
  postProcessor.CopyToCandidate(
    copySlot->GetGfxList(), candidate.resource.Get(),
    copySrcResource.Get());
  copySlot->EndTiming();

  if (!copySlot->Execute())
  {
    if (!copySlot->HasSubmittedWork())
    {
      if (computeSlot)
        m_dx12Device->WaitForIdle();
      ReleaseCandidate(candidateIndex);
    }
    SetFullPendingDamage();
    m_devContext->ForceFrame();
    return false;
  }

  return true;
}

DWORD CALLBACK CSwapChainProcessor::_CursorThread(LPVOID arg)
{
  reinterpret_cast<CSwapChainProcessor*>(arg)->CursorThread();
  return 0;
}

bool CSwapChainProcessor::QueryHWCursor()
{
  IDARG_IN_QUERY_HWCURSOR in = {};
  in.LastShapeId            = m_lastShapeId;
  in.pShapeBuffer           = m_shapeBuffer;
  in.ShapeBufferSizeInBytes = 512 * 512 * 4;

  IDARG_OUT_QUERY_HWCURSOR out = {};
  UINT cursorWhiteLevel = m_sdrWhiteLevel.load(std::memory_order_relaxed);
  NTSTATUS status;
#ifdef HAS_IDDCX_110
  if (m_devContext->HasIddCx110DDIs())
  {
    IDARG_OUT_QUERY_HWCURSOR3 out3 = {};
    status = IddCxMonitorQueryHardwareCursor3(m_monitor, &in, &out3);
    out.IsCursorVisible      = out3.IsCursorVisible;
    out.X                    = out3.X;
    out.Y                    = out3.Y;
    out.IsCursorShapeUpdated = out3.IsCursorShapeUpdated;
    out.CursorShapeInfo      = out3.CursorShapeInfo;
    if (out3.SdrWhiteLevel)
      cursorWhiteLevel = out3.SdrWhiteLevel;
  }
  else
#endif
  {
    status = IddCxMonitorQueryHardwareCursor(m_monitor, &in, &out);
  }

  if (FAILED(status))
  {
    // this occurs if the display went away (ie, screen blanking or disabled)
    if (status == STATUS_GRAPHICS_PATH_NOT_IN_TOPOLOGY)
    {
      SetEvent(m_terminateEvent.Get());
      return false;
    }

    DEBUG_ERROR("IddCxMonitorQueryHardwareCursor failed (0x%08x)", status);
    return false;
  }

  if (out.IsCursorShapeUpdated)
    m_lastShapeId = out.CursorShapeInfo.ShapeId;

  m_devContext->SendCursor(out, m_shapeBuffer, cursorWhiteLevel);
  return true;
}

void CSwapChainProcessor::CursorThread()
{
  HRESULT hr = 0;
  bool running = true;

  while (running)
  {
    HANDLE waitHandles[] =
    {
      m_cursorDataEvent.Get(),
      m_terminateEvent.Get()
    };

    DWORD waitResult = WaitForMultipleObjects(
      ARRAYSIZE(waitHandles), waitHandles, FALSE, 100);

    switch (waitResult)
    {
    case WAIT_TIMEOUT:
      continue;

      // cursorDataEvent
    case WAIT_OBJECT_0:
      if (!QueryHWCursor())
        return;
      continue;

      // terminateEvent
    case WAIT_OBJECT_0 + 1:
      running = false;
      continue;

    default:
      hr = HRESULT_FROM_WIN32(waitResult);
      DEBUG_ERROR_HR(hr, "WaitForMultipleObjects");
      return;
    }
  }
}
