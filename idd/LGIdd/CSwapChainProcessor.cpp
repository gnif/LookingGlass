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

#include <avrt.h>
#include "CDebug.h"
#include "CPipeServer.h"

static const uint32_t HDR_PQ_MIN_LUMINANCE = 50;
static const uint32_t HDR_PQ_MAX_LUMINANCE = 10000;

static_assert(LGMP_Q_FRAME_LEN == 2,
  "IDD damage repair assumes two alternating frame buffers");

static uint64_t Nanotime()
{
  static const uint64_t frequency = []()
  {
    LARGE_INTEGER value;
    QueryPerformanceFrequency(&value);
    return (uint64_t)value.QuadPart;
  }();

  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  const uint64_t ticks = (uint64_t)counter.QuadPart;
  return ticks / frequency * 1000000000ULL +
    ticks % frequency * 1000000000ULL / frequency;
}

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
    CIndirectDeviceContext* devContext, IDDCX_SWAPCHAIN hSwapChain,
    std::shared_ptr<CD3D11Device> dx11Device, std::shared_ptr<CD3D12Device> dx12Device, HANDLE newFrameEvent) :
  m_monitorContext(monitorContext),
  m_assignmentGeneration(assignmentGeneration),
  m_monitor(monitor),
  m_devContext(devContext),
  m_hSwapChain(hSwapChain),
  m_dx11Device(dx11Device),
  m_dx12Device(dx12Device),
  m_newFrameEvent(newFrameEvent)
{
  m_resPool.Init(dx11Device, dx12Device);
  m_fbPool.Init(this);
  if (m_dx11Device->IsSoftware())
    DEBUG_INFO("Software render adapter: post-processing disabled");
  else
  {
    bool initialized = true;
    for (CPostProcessor& postProcessor : m_postProcessors)
      if (!postProcessor.Init(dx12Device))
      {
        initialized = false;
        break;
      }

    if (initialized)
      for (unsigned i = 1; i < ARRAYSIZE(m_postProcessors); ++i)
        if (!m_postProcessors[0].HasSameEffectChain(m_postProcessors[i]))
        {
          DEBUG_ERROR("Post processor effect chains do not match");
          initialized = false;
          break;
        }

    if (!initialized)
    {
      for (CPostProcessor& postProcessor : m_postProcessors)
        postProcessor.Reset();
      DEBUG_ERROR("Failed to initialize post processors");
    }
  }

  // Manual-reset: both worker threads wait on this, so it must stay signalled
  // once set or only one thread would ever observe termination.
  m_terminateEvent.Attach(CreateEvent(nullptr, TRUE, FALSE, nullptr));
  m_cursorDataEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
  m_shapeBuffer = new BYTE[512 * 512 * 4];

  // Start the worker only after every object it can access is initialized.
  m_thread[0].Attach(CreateThread(nullptr, 0, _SwapChainThread, this, 0, nullptr));
}

CSwapChainProcessor::~CSwapChainProcessor()
{
  SetEvent(m_terminateEvent.Get());
  if (m_thread[0].Get())
    WaitForSingleObject(m_thread[0].Get(), INFINITE);
  if (m_thread[1].Get())
    WaitForSingleObject(m_thread[1].Get(), INFINITE);

  // Drain in-flight GPU work / completion callbacks before releasing the
  // resources they reference. The swap chain was already released in the
  // worker epilogue, so this does not hold an IddCx frame.
  m_dx12Device->WaitForIdle();

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

void CSwapChainProcessor::SwapChainThread()
{
  DWORD  avTask       = 0;
  HANDLE avTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &avTask);

  DEBUG_INFO("Start Thread");

  // Only delete the swap chain if we took ownership of it (SetDevice
  // succeeded). If SetDevice failed IddCx still owns and tears it down, so
  // deleting it here would double-free the WDF object. Releasing it when we do
  // own it hands the acquired frame back to IddCx promptly.
  if (SwapChainThreadCore())
    WdfObjectDelete((WDFOBJECT)m_hSwapChain);
  m_hSwapChain = nullptr;

  AvRevertMmThreadCharacteristics(avTaskHandle);
}

bool CSwapChainProcessor::SwapChainThreadCore()
{
  ComPtr<IDXGIDevice> dxgiDevice;
  HRESULT hr = m_dx11Device->GetDevice().As(&dxgiDevice);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to get the dxgiDevice");
    return false;
  }

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

  IDARG_IN_SWAPCHAINSETDEVICE setDevice = {};
  setDevice.pDevice = dxgiDevice.Get();

  // IddCx can unassign a swap chain while its devices are still being
  // created. In that case the owner signals termination and IddCx retains
  // responsibility for the handle because SetDevice has not succeeded.
  if (!m_monitorContext->IsAssignmentCurrent(m_assignmentGeneration) ||
      WaitForSingleObject(m_terminateEvent.Get(), 0) == WAIT_OBJECT_0)
    return false;

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
    return false;
  }
  // Past this point SetDevice succeeded: we own the swap chain and are
  // responsible for deleting it.

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
    return true;
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
    const uint64_t captureStart = Nanotime();

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

  return true;
}

void CSwapChainProcessor::CompletionFunction(
  CD3D12CommandSlot * slot, bool result, void * param1, void * param2)
{
  auto sc    = (CSwapChainProcessor   *)param1;
  auto fbRes = (CFrameBufferResource *)param2;

  if (!result)
  {
    // A submitted frame may already be in LGMP, or publication may race this
    // callback. Make the message releasable even though its contents failed.
    sc->m_devContext->FailFrameBuffer(fbRes->GetFrameIndex());
    return;
  }

  const uint64_t cpuCopyStart = fbRes->GetCopyStart();
  uint64_t       gpuCopyStart = 0;
  uint64_t       gpuCopyEnd   = 0;

  if (sc->m_dx12Device->IsIndirectCopy())
    sc->m_devContext->WriteFrameBuffer(
      fbRes->GetFrameIndex(), fbRes->GetMap(), 0, fbRes->GetFrameSize(), false);

  // Queue waits execute before the start timestamp. The end timestamp follows
  // the last CopyTextureRegion, separating GPU work from readiness dispatch.
  const bool gpuTimingValid =
    slot->GetGPUTimes(gpuCopyStart, gpuCopyEnd);

  // Publish readiness before sampling the endpoint. Timing has its own valid
  // flag and is published immediately afterwards.
  sc->m_devContext->FinalizeFrameBuffer(fbRes->GetFrameIndex());
  const uint64_t readyEnd = Nanotime();

  uint64_t postProcessTime = cpuCopyStart - fbRes->GetPostProcessStart();
  uint64_t copyTime        = readyEnd - cpuCopyStart;
  uint64_t readyTime       = 0;
  if (gpuTimingValid &&
      gpuCopyStart >= fbRes->GetPostProcessStart() &&
      gpuCopyEnd <= readyEnd)
  {
    postProcessTime = gpuCopyStart - fbRes->GetPostProcessStart();
    copyTime        = gpuCopyEnd - gpuCopyStart;
    readyTime       = readyEnd - gpuCopyEnd;
  }

  sc->m_devContext->SetFrameTiming(fbRes->GetFrameIndex(),
    fbRes->GetCaptureTime(), postProcessTime, copyTime, readyTime);
  sc->m_devContext->CompleteFrameBuffer(fbRes->GetFrameIndex());
}


static bool IsFullDamage(const RECT * dirtyRects, unsigned nbDirtyRects,
  const D3D12_RESOURCE_DESC& desc)
{
  for (const RECT * rect = dirtyRects;
       rect < dirtyRects + nbDirtyRects; ++rect)
    if (rect->left   == 0                &&
        rect->top    == 0                &&
        rect->right  == (LONG)desc.Width &&
        rect->bottom == (LONG)desc.Height)
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
  unsigned nbDirtyRects, const D3D12_RESOURCE_DESC& desc)
{
  const uint64_t frameArea = (uint64_t)desc.Width * desc.Height;
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

static void CopyDirtyRect(ComPtr<ID3D12GraphicsCommandList> list,
  D3D12_TEXTURE_COPY_LOCATION * dstLoc,
  D3D12_TEXTURE_COPY_LOCATION * srcLoc,
  const RECT& rect)
{
  D3D12_BOX box = {};
  box.left   = rect.left;
  box.top    = rect.top;
  box.front  = 0;
  box.right  = rect.right;
  box.bottom = rect.bottom;
  box.back   = 1;

  list->CopyTextureRegion(dstLoc, box.left, box.top, 0, srcLoc, &box);
}

static bool ClipDirtyRect(RECT& rect, const D3D12_RESOURCE_DESC& desc)
{
  const LONG maxRight  = (LONG)desc.Width;
  const LONG maxBottom = (LONG)desc.Height;

  if (rect.left   < 0        ) rect.left   = 0;
  if (rect.top    < 0        ) rect.top    = 0;
  if (rect.right  > maxRight ) rect.right  = maxRight;
  if (rect.bottom > maxBottom) rect.bottom = maxBottom;

  return rect.left < rect.right && rect.top < rect.bottom;
}

static void ClipDirtyRects(RECT dirtyRects[], unsigned * nbDirtyRects,
  const D3D12_RESOURCE_DESC& desc)
{
  unsigned out = 0;
  for (unsigned i = 0; i < *nbDirtyRects; ++i)
  {
    RECT rect = dirtyRects[i];
    if (ClipDirtyRect(rect, desc))
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

void CSwapChainProcessor::SetFullPendingDamage()
{
  m_hasPendingDamage    = true;
  m_nbPendingDirtyRects = 0;
}

void CSwapChainProcessor::AccumulateFrameDamage(
  const RECT * dirtyRects, unsigned nbDirtyRects)
{
  if (nbDirtyRects > LG_MAX_DIRTY_RECTS)
    nbDirtyRects = 0;

  if (!m_hasPendingDamage)
  {
    m_hasPendingDamage    = true;
    m_nbPendingDirtyRects = nbDirtyRects;
    if (nbDirtyRects)
      memcpy(m_pendingDirtyRects, dirtyRects,
        nbDirtyRects * sizeof(*m_pendingDirtyRects));
    return;
  }

  // Zero dirty rectangles represents full-frame damage. Once any skipped
  // frame requires a full update, no later rectangles can narrow it again.
  if (m_nbPendingDirtyRects == 0 || nbDirtyRects == 0)
  {
    m_nbPendingDirtyRects = 0;
    return;
  }

  if (m_nbPendingDirtyRects + nbDirtyRects > LG_MAX_DIRTY_RECTS)
  {
    m_nbPendingDirtyRects = 0;
    return;
  }

  memcpy(m_pendingDirtyRects + m_nbPendingDirtyRects, dirtyRects,
    nbDirtyRects * sizeof(*m_pendingDirtyRects));
  m_nbPendingDirtyRects += nbDirtyRects;
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
  const uint64_t postProcessStart = Nanotime();
  const uint64_t captureTime      = postProcessStart - captureStart;

  // Preserve the fast drop path: never hold an IddCx frame while waiting for
  // a slow or disconnected client. We have not read its rectangles, so force
  // the next published frame to invalidate the entire image.
  if (!m_devContext->FrameBufferAvailable())
  {
    SetFullPendingDamage();
    return true;
  }

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

  /**
   * Even though we have not performed any copy/draw operations we still need to
   * use a fence. Because we share this texture with DirectX12 it is able to
   * read from it before the desktop duplication API has finished updating it.
   */
  if (!srcRes->Signal())
  {
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
    AccumulateFrameDamage(
      srcRes->GetDirtyRects(), srcRes->GetDirtyRectCount());

  // Never hold an IddCx frame waiting for a slow or disconnected client. Read
  // and retain its damage first so the next published frame remains complete.
  if (!m_devContext->FrameBufferAvailable())
    return true;

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

  const bool frameMetadataChanged = noImageUpdate &&
    FrameMetadataChanged(m_postProcessors[0].GetOutputFormat(), srcFormat);

  bool needsReconfigure = false;
  for (const CPostProcessor& postProcessor : m_postProcessors)
    if (postProcessor.NeedsReconfigure(srcFormat))
    {
      needsReconfigure = true;
      break;
    }

  // SetFormat can replace resources still being read by the COPY queue. Format
  // changes are rare, so drain both queues before updating either frame chain.
  if (needsReconfigure)
  {
    m_nbDirtyRects = 0;
    SetFullPendingDamage();
    m_dx12Device->WaitForIdle();
  }

  bool postProcessFormatChanged = false;
  for (unsigned i = 0; i < ARRAYSIZE(m_postProcessors); ++i)
  {
    bool formatChanged = false;
    if (!m_postProcessors[i].Configure(srcFormat, &formatChanged))
    {
      SetFullPendingDamage();
      return false;
    }

    if (i == 0)
      postProcessFormatChanged = formatChanged;
  }

  if (postProcessFormatChanged)
  {
    m_nbDirtyRects = 0;
    SetFullPendingDamage();
  }
  else if (frameMetadataChanged)
    SetFullPendingDamage();

  if (noImageUpdate && !m_hasPendingDamage)
    return true;

  const D12FrameFormat& dstFormat =
    m_postProcessors[0].GetOutputFormat();

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
  m_dx12Device->GetDevice()->GetCopyableFootprints(
    &dstFormat.desc,
    0,
    1,
    0,
    &layout,
    NULL,
    NULL,
    NULL);

  RECT currentDirtyRects[LG_MAX_DIRTY_RECTS] = {};
  RECT frameDirtyRects[LG_MAX_DIRTY_RECTS] = {};
  unsigned nbDirtyRects = m_nbPendingDirtyRects;
  if (nbDirtyRects)
  {
    memcpy(currentDirtyRects, m_pendingDirtyRects, nbDirtyRects * sizeof(*currentDirtyRects));
    memcpy(frameDirtyRects, currentDirtyRects, nbDirtyRects * sizeof(*frameDirtyRects));
  }
  unsigned frameDirtyRectCount = nbDirtyRects;
  m_postProcessors[0].AdjustFrameDamage(
    frameDirtyRects, &frameDirtyRectCount);

  auto buffer = m_devContext->PrepareFrameBuffer(
    (unsigned)layout.Footprint.RowPitch,
    srcFormat,
    dstFormat,
    frameDirtyRects,
    frameDirtyRectCount);

  // Queue or framebuffer ownership can change after the early availability
  // check. Treat this as a dropped frame rather than an error.
  if (!buffer.mem)
    return true;

  CFrameBufferResource * fbRes = m_fbPool.Get(buffer,
    (size_t)layout.Footprint.RowPitch * dstFormat.desc.Height);

  if (!fbRes)
  {
    m_devContext->AbortFrameBuffer(buffer.frameIndex);
    DEBUG_ERROR("Failed to get a CFrameBufferResource from the pool");
    SetFullPendingDamage();
    return false;
  }

  CPostProcessor& postProcessor = m_postProcessors[buffer.frameIndex];

  CD3D12CommandSlot * copySlot =
    m_dx12Device->GetCopySlot(buffer.frameIndex);
  if (!copySlot)
  {
    m_devContext->AbortFrameBuffer(buffer.frameIndex);
    DEBUG_ERROR("Failed to get a copy CommandSlot");
    SetFullPendingDamage();
    return false;
  }

  ComPtr<ID3D12Resource> copySrcResource = srcRes->GetRes();
  CD3D12CommandSlot    * computeSlot     = nullptr;
  if (postProcessor.HasActiveEffects())
  {
    computeSlot = m_dx12Device->GetComputeSlot(buffer.frameIndex);
    if (!computeSlot)
    {
      copySlot->Cancel();
      m_devContext->AbortFrameBuffer(buffer.frameIndex);
      DEBUG_ERROR("Failed to get a compute CommandSlot");
      SetFullPendingDamage();
      return false;
    }

    if (!srcRes->Sync(*computeSlot))
    {
      computeSlot->Cancel();
      copySlot->Cancel();
      m_devContext->AbortFrameBuffer(buffer.frameIndex);
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
      m_devContext->AbortFrameBuffer(buffer.frameIndex);
      DEBUG_ERROR("Post processor returned no output resource");
      SetFullPendingDamage();
      return false;
    }

    if (!computeSlot->Execute())
    {
      copySlot->Cancel();
      m_dx12Device->WaitForIdle();
      m_devContext->AbortFrameBuffer(buffer.frameIndex);
      SetFullPendingDamage();
      return false;
    }

    if (!copySlot->WaitFor(*computeSlot))
    {
      copySlot->Cancel();
      m_dx12Device->WaitForIdle();
      m_devContext->AbortFrameBuffer(buffer.frameIndex);
      DEBUG_ERROR("Failed to queue compute synchronization");
      SetFullPendingDamage();
      return false;
    }
  }
  else if (!srcRes->Sync(*copySlot))
  {
    copySlot->Cancel();
    m_devContext->AbortFrameBuffer(buffer.frameIndex);
    DEBUG_ERROR("Failed to queue source synchronization");
    SetFullPendingDamage();
    return false;
  }

  ClipDirtyRects(currentDirtyRects, &nbDirtyRects, dstFormat.desc);

  const uint64_t copyStart = Nanotime();
  fbRes->SetTiming(captureTime, postProcessStart, copyStart);

  copySlot->SetCompletionCallback(&CompletionFunction, this, fbRes);

  D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
  srcLoc.pResource        = copySrcResource.Get();
  srcLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  srcLoc.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
  dstLoc.pResource       = fbRes->Get().Get();
  dstLoc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dstLoc.PlacedFootprint = layout;

  /* Each destination is reused every other frame, so repair both the prior
   * and current damage. Coalesce them first to avoid copying overlapping
   * regions, especially a prior full frame, more than once. */
  RECT     copyDirtyRects[LG_MAX_DIRTY_RECTS * 2] = {};
  unsigned nbCopyDirtyRects                       = 0;
  bool     fullCopy                               =
    nbDirtyRects == 0 || m_nbDirtyRects == 0;

  if (!fullCopy)
  {
    for (const RECT * rect = m_dirtyRects;
         rect < m_dirtyRects + m_nbDirtyRects && !fullCopy; ++rect)
    {
      RECT clipped = *rect;
      if (ClipDirtyRect(clipped, dstFormat.desc) &&
          !AddCopyDirtyRect(copyDirtyRects, ARRAYSIZE(copyDirtyRects),
            &nbCopyDirtyRects, clipped))
        fullCopy = true;
    }

    for (const RECT * rect = currentDirtyRects;
         rect < currentDirtyRects + nbDirtyRects && !fullCopy; ++rect)
      if (!AddCopyDirtyRect(copyDirtyRects, ARRAYSIZE(copyDirtyRects),
          &nbCopyDirtyRects, *rect))
        fullCopy = true;

    if (!fullCopy)
      fullCopy = IsFullDamage(
          copyDirtyRects, nbCopyDirtyRects, dstFormat.desc) ||
        CopyAreaCoversFrame(
          copyDirtyRects, nbCopyDirtyRects, dstFormat.desc);
  }

  // Source/compute waits are submitted immediately before this command list.
  // The timestamp therefore marks the first actual copy operation.
  copySlot->BeginTiming();
  if (fullCopy)
  {
    copySlot->GetGfxList()->CopyTextureRegion(
      &dstLoc, 0, 0, 0, &srcLoc, NULL);
  }
  else
  {
    for (const RECT * rect = copyDirtyRects;
         rect < copyDirtyRects + nbCopyDirtyRects; ++rect)
      CopyDirtyRect(copySlot->GetGfxList(), &dstLoc, &srcLoc, *rect);
  }
  copySlot->EndTiming();

  if (!copySlot->Execute())
  {
    if (!copySlot->HasSubmittedWork())
    {
      if (computeSlot)
        m_dx12Device->WaitForIdle();
      m_devContext->AbortFrameBuffer(buffer.frameIndex);
    }
    SetFullPendingDamage();
    return false;
  }

  if (!m_devContext->PublishFrameBuffer(buffer.frameIndex))
  {
    SetFullPendingDamage();
    return false;
  }

  memcpy(m_dirtyRects, currentDirtyRects,
    nbDirtyRects * sizeof(*m_dirtyRects));
  m_nbDirtyRects        = nbDirtyRects;
  m_hasPendingDamage    = false;
  m_nbPendingDirtyRects = 0;

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
