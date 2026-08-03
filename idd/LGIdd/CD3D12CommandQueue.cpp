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

#include "CD3D12CommandQueue.h"
#include "CDebug.h"

static uint64_t ScaleTicks(uint64_t ticks, uint64_t targetFrequency,
  uint64_t sourceFrequency)
{
  return ticks / sourceFrequency * targetFrequency +
    ticks % sourceFrequency * targetFrequency / sourceFrequency;
}

static uint64_t TicksToNanoseconds(uint64_t ticks, uint64_t frequency)
{
  return ScaleTicks(ticks, 1000000000ULL, frequency);
}

bool CD3D12CommandQueue::InitTiming(ID3D12Device3 * device,
  D3D12_COMMAND_LIST_TYPE type)
{
  if (type != D3D12_COMMAND_LIST_TYPE_COPY)
    return false;

  D3D12_FEATURE_DATA_D3D12_OPTIONS3 options = {};
  HRESULT hr = device->CheckFeatureSupport(
    D3D12_FEATURE_D3D12_OPTIONS3, &options, sizeof(options));
  if (FAILED(hr) || !options.CopyQueueTimestampQueriesSupported)
    return false;

  hr = m_queue->GetTimestampFrequency(&m_timestampFrequency);
  if (FAILED(hr) || !m_timestampFrequency)
    return false;

  LARGE_INTEGER qpcFrequency;
  if (!QueryPerformanceFrequency(&qpcFrequency))
    return false;
  m_qpcFrequency = (UINT64)qpcFrequency.QuadPart;

  D3D12_QUERY_HEAP_DESC queryDesc = {};
  queryDesc.Type  = D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP;
  queryDesc.Count = 1;

  hr = device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_timestampHeap));
  if (FAILED(hr))
    return false;

  D3D12_HEAP_PROPERTIES heapProps = {};
  heapProps.Type                 = D3D12_HEAP_TYPE_READBACK;
  heapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  heapProps.CreationNodeMask     = 1;
  heapProps.VisibleNodeMask      = 1;

  D3D12_RESOURCE_DESC resourceDesc = {};
  resourceDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
  resourceDesc.Width              = sizeof(UINT64);
  resourceDesc.Height             = 1;
  resourceDesc.DepthOrArraySize   = 1;
  resourceDesc.MipLevels          = 1;
  resourceDesc.SampleDesc.Count   = 1;
  resourceDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  hr = device->CreateCommittedResource(
    &heapProps,
    D3D12_HEAP_FLAG_NONE,
    &resourceDesc,
    D3D12_RESOURCE_STATE_COPY_DEST,
    NULL,
    IID_PPV_ARGS(&m_timestampReadback));
  if (FAILED(hr))
  {
    m_timestampHeap.Reset();
    return false;
  }

  D3D12_RANGE readRange    = { 0, sizeof(UINT64) };
  void      * timestampMap = nullptr;
  hr = m_timestampReadback->Map(0, &readRange, &timestampMap);
  if (FAILED(hr))
  {
    m_timestampReadback.Reset();
    m_timestampHeap.Reset();
    return false;
  }
  m_timestampMap = static_cast<UINT64 *>(timestampMap);

  hr = m_queue->GetClockCalibration(&m_calibrationGPU, &m_calibrationCPU);
  if (FAILED(hr))
  {
    D3D12_RANGE writeRange = { 0, 0 };
    m_timestampReadback->Unmap(0, &writeRange);
    m_timestampMap = nullptr;
    m_timestampReadback.Reset();
    m_timestampHeap.Reset();
    return false;
  }

  m_timingSupported = true;
  return true;
}

void CD3D12CommandQueue::UpdateClockCalibration()
{
  if (!m_timingSupported)
    return;

  LARGE_INTEGER now;
  if (QueryPerformanceCounter(&now) &&
      (UINT64)now.QuadPart >= m_calibrationCPU &&
      (UINT64)now.QuadPart - m_calibrationCPU < m_qpcFrequency)
    return;

  UINT64 gpu;
  UINT64 cpu;
  if (SUCCEEDED(m_queue->GetClockCalibration(&gpu, &cpu)))
  {
    m_calibrationGPU = gpu;
    m_calibrationCPU = cpu;
  }
}

bool CD3D12CommandQueue::Init(ID3D12Device3 * device,
  D3D12_COMMAND_LIST_TYPE type, const WCHAR * name,
  CallbackMode callbackMode)
{
  HRESULT hr;
  D3D12_COMMAND_QUEUE_DESC queueDesc = {};

  queueDesc.Type     = type;
  queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
  queueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;

  hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create the CommandQueue (%ls)", name);
    return false;
  }
  m_queue->SetName(name);

  hr = device->CreateCommandAllocator(type, IID_PPV_ARGS(&m_allocator));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create the CommandAllocator (%ls)", name);
    return false;
  }

  hr = device->CreateCommandList(0, type, m_allocator.Get(), NULL, IID_PPV_ARGS(&m_gfxList));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create the Graphics CommandList (%ls)", name);
    return false;
  }
  m_gfxList->SetName(name);

  m_cmdList = m_gfxList;
  if (!m_cmdList)
  {
    DEBUG_ERROR_HR(hr, "Failed to get the CommandList (%ls)", name);
    return false;
  }

  hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create the Fence (%ls)", name);
    return false;
  }

  m_event.Attach(CreateEvent(NULL, FALSE, FALSE, NULL));
  if (m_event.Get() == INVALID_HANDLE_VALUE)
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create the completion event (%ls)", name);
    return false;
  }

  if (callbackMode != DISABLED)
  {
    ULONG flags = (callbackMode == FAST) ?
      WT_EXECUTEINWAITTHREAD : WT_EXECUTEINPERSISTENTTHREAD;

    if (!RegisterWaitForSingleObject(
      &m_waitHandle,
      m_event.Get(),
      [](PVOID param, BOOLEAN timeout){
        CD3D12CommandQueue * queue = (CD3D12CommandQueue*)param;
        if (timeout)
          queue->m_completionResult = false;
        queue->OnCompletion();
      },
      this,
      INFINITE,
      flags))
    {
      DEBUG_ERROR_HR(GetLastError(),
        "Failed to register the completion wait (%ls)", name);
      m_waitHandle = INVALID_HANDLE_VALUE;
      return false;
    }
  }

  if (callbackMode != DISABLED &&
      type == D3D12_COMMAND_LIST_TYPE_COPY && !InitTiming(device, type))
    DEBUG_WARN("GPU timing is unavailable for CommandQueue(%ls)", name);

  m_name       = name;
  m_fenceValue = 0;
  DEBUG_INFO("Created CD3D12CommandQueue(%ls)", name);
  return true;
}

void CD3D12CommandQueue::DeInit()
{
  if (m_waitHandle != INVALID_HANDLE_VALUE)
  {
    // Queue owners drain callbacks before destruction. Keep this unregister
    // non-blocking so a removed or hung device cannot stall teardown.
    UnregisterWait(m_waitHandle);
    m_waitHandle = INVALID_HANDLE_VALUE;
  }

  if (m_timestampMap)
  {
    D3D12_RANGE writeRange = { 0, 0 };
    m_timestampReadback->Unmap(0, &writeRange);
    m_timestampMap = nullptr;
  }
}

bool CD3D12CommandQueue::Execute()
{
  m_needsReset       = true;
  m_completionResult = true;
  m_pending          = m_waitHandle != INVALID_HANDLE_VALUE;

  HRESULT hr = m_gfxList->Close();
  if (FAILED(hr))
  {
    m_completionResult = false;
    SetEvent(m_event.Get());

    DEBUG_ERROR_HR(hr, "Failed to close the command list (%ls)", m_name);
    return false;
  }

  ID3D12CommandList * lists[] = { m_cmdList.Get() };
  m_queue->ExecuteCommandLists(1, lists);
  ++m_fenceValue;

  hr = m_fence->SetEventOnCompletion(m_fenceValue, m_event.Get());
  if (FAILED(hr))
  {
    m_completionResult = false;
    SetEvent(m_event.Get());

    DEBUG_ERROR_HR(hr, "Failed to set the fence signal (%ls)", m_name);
    return false;
  }

  m_queue->Signal(m_fence.Get(), m_fenceValue);
  return true;
}

bool CD3D12CommandQueue::BeginTiming()
{
  m_timingActive = m_timingSupported;
  if (!m_timingActive)
    return false;

  // Refresh after an idle period; active queues are calibrated by their
  // completion path without adding work to frame submission.
  UpdateClockCalibration();
  m_gfxList->EndQuery(
    m_timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
  return true;
}

void CD3D12CommandQueue::EndTiming()
{
  if (!m_timingActive)
    return;

  m_gfxList->ResolveQueryData(
    m_timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
    0, 1, m_timestampReadback.Get(), 0);
}

bool CD3D12CommandQueue::GetGPUStartTime(uint64_t& start)
{
  if (!m_timingActive)
    return false;

  const UINT64 gpuStart = m_timestampMap[0];

  UINT64 cpuStart;
  if (gpuStart < m_calibrationGPU)
  {
    const UINT64 delta = ScaleTicks(
      m_calibrationGPU - gpuStart, m_qpcFrequency, m_timestampFrequency);
    if (delta > m_calibrationCPU)
      return false;
    cpuStart = m_calibrationCPU - delta;
  }
  else
  {
    const UINT64 delta = ScaleTicks(
      gpuStart - m_calibrationGPU, m_qpcFrequency, m_timestampFrequency);
    if (UINT64_MAX - m_calibrationCPU < delta)
      return false;
    cpuStart = m_calibrationCPU + delta;
  }

  start = TicksToNanoseconds(cpuStart, m_qpcFrequency);
  return true;
}

#if 0
void CD3D12CommandQueue::Wait()
{
  if (m_fence->GetCompletedValue() >= m_fenceValue)
  {
    m_pending = false;
    return;
  }

  m_fence->SetEventOnCompletion(m_fenceValue, m_event.Get());
  WaitForSingleObject(m_event.Get(), INFINITE);
  m_pending = false;
}
#endif

bool CD3D12CommandQueue::Reset()
{
  m_timingActive = false;
  if (!m_needsReset)
    return true;

  HRESULT hr;

  hr = m_allocator->Reset();
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to reset the command allocator (%ls)", m_name);
    return false;
  }

  hr = m_gfxList->Reset(m_allocator.Get(), NULL);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to reset the graphics command list (%ls)", m_name);
    return false;
  }

  m_needsReset = false;
  return true;
}
