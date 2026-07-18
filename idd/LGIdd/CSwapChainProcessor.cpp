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
  else if (!m_postProcessor.Init(dx12Device))
    DEBUG_ERROR("Failed to initialize post processor");

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

  m_postProcessor.Reset();
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

    UINT frameNumber = 0;
    UINT dirtyRectCount = 0;
    ComPtr<IDXGIResource> surface;

    // The surface colour space is the source of truth for the content format.
    // Only the buffer2 acquisition path (IddCx 1.10+) reports it; on the legacy
    // path HDR is not available, so default to SDR.
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT sdrWhiteLevel = KVMFR_SDR_WHITE_LEVEL_DEFAULT;

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
        frameNumber = buffer.MetaData.PresentationFrameNumber;
        dirtyRectCount = buffer.MetaData.DirtyRectCount;
        surface = buffer.MetaData.pSurface;
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
        if (!SwapChainNewFrame(surface, dirtyRectCount, colorSpace, sdrWhiteLevel))
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
  CD3D12CommandQueue * queue, bool result, void * param1, void * param2)
{
  UNREFERENCED_PARAMETER(queue);

  auto sc    = (CSwapChainProcessor *)param1;
  auto fbRes = (CFrameBufferResource*)param2;

  // fail gracefully
  if (!result)
  {
    sc->m_devContext->FinalizeFrameBuffer(fbRes->GetFrameIndex());
    return;
  }

  if (sc->m_dx12Device->IsIndirectCopy())
    sc->m_devContext->WriteFrameBuffer(
      fbRes->GetFrameIndex(),
      fbRes->GetMap(), 0, fbRes->GetFrameSize(), true);
  else
    sc->m_devContext->FinalizeFrameBuffer(fbRes->GetFrameIndex());
}


static bool IsFullDamage(const RECT * dirtyRects, unsigned nbDirtyRects,
  const D3D12_RESOURCE_DESC& desc)
{
  return nbDirtyRects == 0 ||
    (nbDirtyRects == 1 &&
      dirtyRects[0].left   == 0 &&
      dirtyRects[0].top    == 0 &&
      dirtyRects[0].right  == (LONG)desc.Width &&
      dirtyRects[0].bottom == (LONG)desc.Height);
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

bool CSwapChainProcessor::GetHDRMetadata(D12FrameFormat& format) const
{
#ifdef HAS_IDDCX_110
  if (m_useDefaultHDRMetadata)
    return m_devContext->GetHDRMetadata(format);

  if (!m_hasNewHDRMetadata)
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

bool CSwapChainProcessor::SwapChainNewFrame(ComPtr<IDXGIResource> acquiredBuffer, unsigned dirtyRectCount,
  DXGI_COLOR_SPACE_TYPE colorSpace, UINT sdrWhiteLevel)
{
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
  srcRes->Signal();

  RECT dirtyRects[LG_MAX_DIRTY_RECTS] = {0};
  if (dirtyRectCount > ARRAYSIZE(dirtyRects))
  {
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
    else
      srcRes->SetDirtyRects(dirtyRects, dirtyOut.DirtyRectOutCount);
  }

  D3D12_RESOURCE_DESC srcDesc = srcRes->GetRes()->GetDesc();
  AccumulateFrameDamage(
    srcRes->GetDirtyRects(), srcRes->GetDirtyRectCount());

  // Never hold an IddCx frame waiting for a slow or disconnected client. Read
  // and retain its damage first so the next published frame remains complete.
  if (!m_devContext->FrameBufferAvailable())
    return true;

  D12FrameFormat srcFormat = {};
  srcFormat.desc          = srcDesc;
  srcFormat.width         = (unsigned)srcDesc.Width;
  srcFormat.height        = srcDesc.Height;
  srcFormat.format        = GetFrameType(srcDesc.Format);
  srcFormat.sdrWhiteLevel = sdrWhiteLevel;
  srcFormat.colorTransform = m_devContext->GetColorTransform();

  switch (colorSpace)
  {
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
      // HDR10: BT.2020 primaries with the PQ (ST.2084) transfer function
      // already applied to the pixel data.
      srcFormat.hdr   = true;
      srcFormat.hdrPQ = true;
      if (!GetHDRMetadata(srcFormat))
      {
        // HDR is active but the OS has not delivered static metadata yet
        // (e.g. a brief window during a mode switch). The pixels are still
        // PQ-encoded, so keep the PQ flag and supply BT.2020/PQ defaults
        // rather than mislabel the frame as SDR.
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
        // Mastering luminances follow SMPTE ST 2086 units: max in whole cd/m²,
        // min in 0.0001 cd/m². 1000 cd/m² display, 0.0001 cd/m² black:
        srcFormat.maxDisplayLuminance = 1000;
        srcFormat.minDisplayLuminance = 1;
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
      if (!GetHDRMetadata(srcFormat))
      {
        // No HDR metadata from the OS; provide reasonable defaults
        // so downstream consumers have valid primaries and luminances.
        // BT.709/sRGB primaries (in 0.00002 units):
        srcFormat.displayPrimary[0][0] = 13250; // Rx
        srcFormat.displayPrimary[0][1] = 34500; // Ry
        srcFormat.displayPrimary[1][0] =  7500; // Gx
        srcFormat.displayPrimary[1][1] = 30000; // Gy
        srcFormat.displayPrimary[2][0] = 34000; // Bx
        srcFormat.displayPrimary[2][1] = 16000; // By
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

  bool postProcessFormatChanged = false;
  if (!m_postProcessor.Configure(srcFormat, &postProcessFormatChanged))
    return false;

  if (postProcessFormatChanged)
  {
    m_nbDirtyRects = 0;
    SetFullPendingDamage();
  }

  const D12FrameFormat& dstFormat = m_postProcessor.GetOutputFormat();

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
  m_postProcessor.AdjustFrameDamage(frameDirtyRects, &frameDirtyRectCount);

  auto copyQueue = m_dx12Device->GetCopyQueue();
  if (!copyQueue)
  {
    DEBUG_ERROR("Failed to get a CopyQueue");
    return false;
  }

  ComPtr<ID3D12Resource> copySrcResource = srcRes->GetRes();
  CD3D12CommandQueue * computeQueue = nullptr;
  if (m_postProcessor.HasActiveEffects())
  {
    computeQueue = m_dx12Device->GetComputeQueue();
    if (!computeQueue)
    {
      DEBUG_ERROR("Failed to get a ComputeQueue");
      return false;
    }

    srcRes->Sync(*computeQueue);
    copySrcResource = m_postProcessor.Run(
      computeQueue->GetGfxList(), copySrcResource,
      currentDirtyRects, &nbDirtyRects);

    if (!computeQueue->Execute())
    {
      SetFullPendingDamage();
      return false;
    }
    copyQueue->WaitFor(*computeQueue);
  }
  else
    srcRes->Sync(*copyQueue);

  ClipDirtyRects(currentDirtyRects, &nbDirtyRects, dstFormat.desc);

  auto buffer = m_devContext->PrepareFrameBuffer(
    (unsigned)layout.Footprint.RowPitch,
    srcFormat,
    dstFormat,
    frameDirtyRects,
    frameDirtyRectCount);

  // The LGMP timer can fill the queue with a subscriber resend after the early
  // availability check. Treat this as a dropped frame rather than an error.
  if (!buffer.mem)
    return true;

  CFrameBufferResource * fbRes = m_fbPool.Get(buffer,
    (size_t)layout.Footprint.RowPitch * dstFormat.desc.Height);

  if (!fbRes)
  {
    DEBUG_ERROR("Failed to get a CFrameBufferResource from the pool");
    SetFullPendingDamage();
    return false;
  }

  copyQueue->SetCompletionCallback(&CompletionFunction, this, fbRes);

  D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
  srcLoc.pResource        = copySrcResource.Get();
  srcLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  srcLoc.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
  dstLoc.pResource       = fbRes->Get().Get();
  dstLoc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dstLoc.PlacedFootprint = layout;

  if (IsFullDamage(currentDirtyRects, nbDirtyRects, dstFormat.desc) ||
      nbDirtyRects > KVMFR_MAX_DAMAGE_RECTS || m_nbDirtyRects == 0)
  {
    copyQueue->GetGfxList()->CopyTextureRegion(
      &dstLoc, 0, 0, 0, &srcLoc, NULL);
  }
  else if (m_nbDirtyRects + nbDirtyRects > LG_MAX_DIRTY_RECTS)
  {
    copyQueue->GetGfxList()->CopyTextureRegion(
      &dstLoc, 0, 0, 0, &srcLoc, NULL);
  }
  else
  {
    for (const RECT * rect = m_dirtyRects; rect < m_dirtyRects + m_nbDirtyRects; ++rect)
    {
      RECT clipped = *rect;
      if (ClipDirtyRect(clipped, dstFormat.desc))
        CopyDirtyRect(copyQueue->GetGfxList(), &dstLoc, &srcLoc, clipped);
    }

    for (const RECT * rect = currentDirtyRects; rect < currentDirtyRects + nbDirtyRects; ++rect)
      CopyDirtyRect(copyQueue->GetGfxList(), &dstLoc, &srcLoc, *rect);
  }

  if (!copyQueue->Execute())
  {
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
