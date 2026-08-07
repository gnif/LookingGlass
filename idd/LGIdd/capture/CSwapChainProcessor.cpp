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

#include "capture/CSwapChainProcessor.h"
#include "capture/CFrameProcessorUtil.h"
#include "display/IddCxCompat.h"
#include "display/device/CDeviceContext.h"
#include "display/monitor/Context.h"
#include "platform/CPlatformInfo.h"
#include "transport/IFrameTransport.h"
#include "transport/IControlTransport.h"
#include "util/CSRWLock.h"

#include <avrt.h>
#include <new>
#include "CDebug.h"
#include "transport/CPipeServer.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
  #define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static const uint32_t HDR_PQ_MIN_LUMINANCE = 50;
static const uint32_t HDR_PQ_MAX_LUMINANCE = 10000;

CSwapChainProcessor::CSwapChainProcessor(CMonitorContext * monitorContext,
    UINT64 assignmentGeneration, IDDCX_MONITOR monitor,
    CDeviceContext * devContext, IDDCX_SWAPCHAIN hSwapChain,
    LUID renderAdapter, std::shared_ptr<CD3D11Device> dx11Device,
    HANDLE newFrameEvent) :
  m_monitorContext(monitorContext),
  m_assignmentGeneration(assignmentGeneration),
  m_monitor(monitor),
  m_devContext(devContext),
  m_transport(devContext->GetTransport().Frames()),
  m_control(devContext->GetTransport().Control()),
  m_hSwapChain(hSwapChain),
  m_renderAdapter(renderAdapter),
  m_dx11Device(dx11Device),
  m_newFrameEvent(newFrameEvent)
{
  // Manual-reset: all worker threads wait on this, so it must stay signalled
  // once set or only one thread would ever observe termination.
  m_terminateEvent.Attach(CreateEvent(nullptr, TRUE, FALSE, nullptr));
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
  if (!m_terminateEvent.Get() || !m_publishTimer.Get() ||
      !m_cursorDataEvent.Get() || !m_shapeBuffer)
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
      m_devContext->GetTransport().GetDirectMemory(), alignSize,
      !m_dx11Device->IsSoftware());
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

    if (!m_devContext->SetupTransport(alignSize))
    {
      DEBUG_ERROR("Transport setup failed");
      return false;
    }

    m_dx12Device = std::move(dx12Device);
    break;
  }

  if (!m_monitorContext->IsAssignmentCurrent(m_assignmentGeneration) ||
      WaitForSingleObject(m_terminateEvent.Get(), 0) == WAIT_OBJECT_0)
    return false;

  m_resPool.Init(m_dx11Device, m_dx12Device);
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

  m_frameProcessor = CreateFrameProcessor(m_dx11Device->IsSoftware(),
    &m_transport, m_dx12Device, m_postProcessors,
    &m_pipelineLock, m_terminateEvent.Get());
  if (!m_frameProcessor)
  {
    DEBUG_ERROR("Failed to create the frame processor");
    return false;
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
    if (m_frameProcessor)
      m_frameProcessor->Reset();
  }

  for (CPostProcessor& postProcessor : m_postProcessors)
    postProcessor.Reset();
  m_frameProcessor.reset();
  m_resPool.Reset();
  delete[] m_shapeBuffer;
}

DWORD CALLBACK CSwapChainProcessor::_SwapChainThread(LPVOID arg)
{
  reinterpret_cast<CSwapChainProcessor*>(arg)->SwapChainThread();
  return 0;
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

  UINT lastFrameNumber    = 0;
  bool hasLastFrameNumber = false;
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
    UINT sdrWhiteLevel = LG_SDR_WHITE_LEVEL_DEFAULT;
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
      const bool duplicateFrame =
        hasLastFrameNumber && frameNumber == lastFrameNumber;
      if (!duplicateFrame)
      {
        lastFrameNumber    = frameNumber;
        hasLastFrameNumber = true;
      }
      if (!SwapChainNewFrame(surface, dirtyRectCount, moveRegionCount,
            colorSpace, sdrWhiteLevel, captureStart, duplicateFrame))
        DEBUG_WARN("Failed to submit frame");

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
  uint64_t captureStart, bool duplicateFrame)
{
  const uint64_t postProcessStart = CFrameScheduler::Nanotime();
  const uint64_t captureTime      = postProcessStart - captureStart;

  RECT     dirtyRects[LG_MAX_DIRTY_RECTS] = {0};
  unsigned resolvedDirtyRectCount         = 0;
  bool     fullDamage                     = false;
  bool     noImageUpdate                  = false;
  HRESULT  hr;
  if (moveRegionCount || dirtyRectCount > ARRAYSIZE(dirtyRects))
  {
    // Move regions are not represented by the dirty rectangle list. Copy the
    // full surface so the alternating destinations remain coherent.
    fullDamage = true;
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
      fullDamage = true;
    }
    else if (dirtyOut.DirtyRectOutCount == 1 &&
        dirtyRects[0].left   == 0 && dirtyRects[0].top    == 0 &&
        dirtyRects[0].right  == 0 && dirtyRects[0].bottom == 0)
    {
      // One empty rectangle is IddCx's static-desktop re-encode marker. It
      // does not describe an image update and must not become full damage.
      noImageUpdate = true;
    }
    else
      resolvedDirtyRectCount = dirtyOut.DirtyRectOutCount;
  }

  // Reencode frames reuse the preceding presentation number. Inspect their
  // empty dirty rectangle above, but suppress every ordinary duplicate.
  if (duplicateFrame && !noImageUpdate)
    return true;

  ComPtr<ID3D11Texture2D> texture;
  hr = acquiredBuffer.As(&texture);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr,
      "Failed to obtain the ID3D11Texture2D from the acquiredBuffer");
    m_frameProcessor->SetFullDamage();
    return false;
  }

  CInteropResource * srcRes = m_resPool.Get(texture);
  if (!srcRes)
  {
    DEBUG_ERROR("Failed to get a CInteropResource from the pool");
    m_frameProcessor->SetFullDamage();
    return false;
  }

  if (fullDamage)
    srcRes->SetFullDamage();
  else
    srcRes->SetDirtyRects(dirtyRects, resolvedDirtyRectCount);

  D3D12_RESOURCE_DESC srcDesc = srcRes->GetRes()->GetDesc();
  if (!noImageUpdate)
  {
    m_transport.ObserveFrame(postProcessStart);
    m_frameProcessor->AccumulateDamage(
      srcRes->GetDirtyRects(), srcRes->GetDirtyRectCount());
  }

  D12FrameFormat srcFormat = {};
  srcFormat.desc           = srcDesc;
  srcFormat.width          = (unsigned)srcDesc.Width;
  srcFormat.height         = srcDesc.Height;
  srcFormat.format         = CFrameProcessorUtil::GetFrameType(srcDesc.Format);
  srcFormat.sdrWhiteLevel  = sdrWhiteLevel;
  srcFormat.colorTransform = m_control.GetColorTransform();

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
      CFrameProcessorUtil::FrameMetadataChanged(
        m_postProcessors[0].GetOutputFormat(), srcFormat);

    for (const CPostProcessor& postProcessor : m_postProcessors)
      if (postProcessor.NeedsReconfigure(srcFormat))
      {
        needsReconfigure = true;
        break;
      }

    // A format change can replace resources referenced by in-flight work.
    // Drain both queues before invalidating the selected frame processor.
    if (needsReconfigure)
    {
      m_dx12Device->WaitForIdle();
      m_frameProcessor->ResetPipeline();
    }

    bool configurationStable = false;
    for (unsigned pass = 0; pass < 2 && !configurationStable; ++pass)
    {
      for (unsigned i = 0; i < ARRAYSIZE(m_postProcessors); ++i)
      {
        bool formatChanged = false;
        if (!m_postProcessors[i].Configure(srcFormat, &formatChanged))
        {
          m_frameProcessor->SetFullDamage();
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
      m_frameProcessor->SetFullDamage();
      return false;
    }

    if (postProcessFormatChanged)
      m_frameProcessor->Invalidate();
    else if (frameMetadataChanged)
      m_frameProcessor->SetFullDamage();

    requiresFullDamage = m_postProcessors[0].RequiresFullDamage();
    if (requiresFullDamage)
      m_frameProcessor->SetFullDamage();

    m_postProcessors[0].GetTimingToken(
      &timingEffectIndex, &timingToken);
  }

  if (needsReconfigure || postProcessFormatChanged || frameMetadataChanged)
    m_transport.ForceFrame();

  const FrameSubmission submission =
  {
    srcRes,
    srcFormat,
    captureTime,
    postProcessStart,
    timingEffectIndex,
    timingToken,
    noImageUpdate,
  };
  return m_frameProcessor->Submit(submission);
}
