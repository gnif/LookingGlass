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

#include <avrt.h>
#include "CDebug.h"
#include "CPipeServer.h"

CSwapChainProcessor::CSwapChainProcessor(IDDCX_MONITOR monitor, CIndirectDeviceContext* devContext, IDDCX_SWAPCHAIN hSwapChain,
    std::shared_ptr<CD3D11Device> dx11Device, std::shared_ptr<CD3D12Device> dx12Device, HANDLE newFrameEvent) :
  m_monitor(monitor),
  m_devContext(devContext),
  m_hSwapChain(hSwapChain),
  m_dx11Device(dx11Device),
  m_dx12Device(dx12Device),
  m_newFrameEvent(newFrameEvent)
{
  m_resPool.Init(dx11Device, dx12Device);
  m_fbPool.Init(this);
  if (!m_postProcessor.Init(dx12Device))
    DEBUG_ERROR("Failed to initialize post processor");

  m_terminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
  m_thread[0].Attach(CreateThread(nullptr, 0, _SwapChainThread, this, 0, nullptr));

  m_cursorDataEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
  m_shapeBuffer = new BYTE[512 * 512 * 4];
}

CSwapChainProcessor::~CSwapChainProcessor()
{
  SetEvent(m_terminateEvent.Get());
  if (m_thread[0].Get())
    WaitForSingleObject(m_thread[0].Get(), INFINITE);
  if (m_thread[1].Get())
    WaitForSingleObject(m_thread[1].Get(), INFINITE);

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
  SwapChainThreadCore();

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

  hr = IddCxSwapChainSetDevice(m_hSwapChain, &setDevice);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "IddCxSwapChainSetDevice Failed");
    return;
  }

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

#if defined(IDDCX_VERSION_MAJOR) && defined(IDDCX_VERSION_MINOR) && \
  (IDDCX_VERSION_MAJOR > 1 || (IDDCX_VERSION_MAJOR == 1 && IDDCX_VERSION_MINOR >= 10))
    if (m_devContext->CanProcessFP16())
    {
      IDARG_IN_RELEASEANDACQUIREBUFFER2 acquireIn = {};
      acquireIn.Size = sizeof(acquireIn);
      acquireIn.AcquireSystemMemoryBuffer = FALSE;

      IDARG_OUT_RELEASEANDACQUIREBUFFER2 buffer = {};
      buffer.MetaData.Size = sizeof(buffer.MetaData);

      hr = IddCxSwapChainReleaseAndAcquireBuffer2(m_hSwapChain, &acquireIn, &buffer);
      if (SUCCEEDED(hr))
      {
        frameNumber = buffer.MetaData.PresentationFrameNumber;
        dirtyRectCount = buffer.MetaData.DirtyRectCount;
        surface = buffer.MetaData.pSurface;
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
        SwapChainNewFrame(surface, dirtyRectCount);

        // report that all GPU processing for this frame has been queued
        hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
        if (FAILED(hr))
        {
          if (hr == STATUS_GRAPHICS_PATH_NOT_IN_TOPOLOGY)
            m_devContext->OnSwapChainLost();
          else
            DEBUG_ERROR_HR(hr, "IddCxSwapChainFinishedProcessingFrame Failed");
          break;
        }

      }
    }
    else
    {
      if (hr == STATUS_GRAPHICS_PATH_NOT_IN_TOPOLOGY)
        m_devContext->OnSwapChainLost();
      break;
    }
  }
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

bool CSwapChainProcessor::SwapChainNewFrame(ComPtr<IDXGIResource> acquiredBuffer, unsigned dirtyRectCount)
{
  ComPtr<ID3D11Texture2D> texture;
  HRESULT hr = acquiredBuffer.As(&texture);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to obtain the ID3D11Texture2D from the acquiredBuffer");
    return false;
  }

  CInteropResource * srcRes = m_resPool.Get(texture);
  if (!srcRes)
  {
    DEBUG_ERROR("Failed to get a CInteropResource from the pool");
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
  D12FrameFormat srcFormat = {};
  srcFormat.desc   = srcDesc;
  srcFormat.width  = (unsigned)srcDesc.Width;
  srcFormat.height = srcDesc.Height;
  srcFormat.format = GetFrameType(srcDesc.Format);

  // Determine HDR status from both format and color space.
  // HDR metadata loading is handled inside each branch so the logic
  // is self-contained: first identify the content type, then load the
  // matching metadata.
  if (srcDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
  {
    // FP16 is HDR content (scRGB / linear, not PQ-curve).
    // FP16 always carries HDR color data regardless of OS HDR mode,
    // but metadata (primaries, luminances) may be unavailable.
    srcFormat.hdr   = true;
    srcFormat.hdrPQ = false;
    if (!m_devContext->GetHDRMetadata(srcFormat))
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
      // 80 cd/m² display, 0.005 cd/m² black (in 0.0001 cd/m² units):
      srcFormat.maxDisplayLuminance = 800000;
      srcFormat.minDisplayLuminance = 50;
      // Content light levels unknown:
      srcFormat.maxContentLightLevel      = 0;
      srcFormat.maxFrameAverageLightLevel = 0;
    }
  }
  else if (m_devContext->CanProcessFP16() && m_devContext->IsHDRActive())
  {
    // Non-FP16 format (e.g., RGBA10) with OS HDR mode active.
    // Windows applies the PQ (ST.2084) transfer function for non-FP16 HDR.
    srcFormat.hdr   = true;
    srcFormat.hdrPQ = true;

    // Load HDR metadata; if none is available the frame is not HDR.
    if (!m_devContext->GetHDRMetadata(srcFormat))
      srcFormat.hdr = false;
  }

  bool postProcessFormatChanged = false;
  if (!m_postProcessor.Configure(srcFormat, &postProcessFormatChanged))
    return false;

  if (postProcessFormatChanged)
    m_nbDirtyRects = 0;

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
  unsigned nbDirtyRects = srcRes->GetDirtyRectCount();
  if (nbDirtyRects > ARRAYSIZE(currentDirtyRects))
    nbDirtyRects = 0;
  else
  {
    memcpy(currentDirtyRects, srcRes->GetDirtyRects(), nbDirtyRects * sizeof(*currentDirtyRects));
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

    computeQueue->Execute();
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

  if (!buffer.mem)
    return false;

  CFrameBufferResource * fbRes = m_fbPool.Get(buffer,
    (size_t)layout.Footprint.RowPitch * dstFormat.desc.Height);

  if (!fbRes)
  {
    DEBUG_ERROR("Failed to get a CFrameBufferResource from the pool");
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

  memcpy(m_dirtyRects, currentDirtyRects, nbDirtyRects * sizeof(*m_dirtyRects));
  m_nbDirtyRects = nbDirtyRects;

  copyQueue->Execute();

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
  NTSTATUS status;
#if defined(IDDCX_VERSION_MAJOR) && defined(IDDCX_VERSION_MINOR) && \
  (IDDCX_VERSION_MAJOR > 1 || (IDDCX_VERSION_MAJOR == 1 && IDDCX_VERSION_MINOR >= 10))
  if (m_devContext->CanProcessFP16())
  {
    IDARG_OUT_QUERY_HWCURSOR3 out3 = {};
    status = IddCxMonitorQueryHardwareCursor3(m_monitor, &in, &out3);
    out.IsCursorVisible      = out3.IsCursorVisible;
    out.X                    = out3.X;
    out.Y                    = out3.Y;
    out.IsCursorShapeUpdated = out3.IsCursorShapeUpdated;
    out.CursorShapeInfo      = out3.CursorShapeInfo;
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
      m_devContext->OnSwapChainLost();
      SetEvent(m_terminateEvent.Get());
      return false;
    }

    DEBUG_ERROR("IddCxMonitorQueryHardwareCursor failed (0x%08x)", status);
    return false;
  }

  if (out.IsCursorShapeUpdated)
    m_lastShapeId = out.CursorShapeInfo.ShapeId;

  m_devContext->SendCursor(out, m_shapeBuffer);
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
