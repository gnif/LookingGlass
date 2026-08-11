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

#include "d3d/CD3D12CommandQueue.h"
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

static bool ConvertGPUTimestamp(UINT64 timestamp, UINT64 timestampFrequency,
  UINT64 calibrationGPU, UINT64 calibrationCPU, UINT64 qpcFrequency,
  uint64_t& result)
{
  UINT64 cpuTimestamp;
  if (timestamp < calibrationGPU)
  {
    const UINT64 delta = ScaleTicks(
      calibrationGPU - timestamp, qpcFrequency, timestampFrequency);
    if (delta > calibrationCPU)
      return false;
    cpuTimestamp = calibrationCPU - delta;
  }
  else
  {
    const UINT64 delta = ScaleTicks(
      timestamp - calibrationGPU, qpcFrequency, timestampFrequency);
    if (UINT64_MAX - calibrationCPU < delta)
      return false;
    cpuTimestamp = calibrationCPU + delta;
  }

  result = TicksToNanoseconds(cpuTimestamp, qpcFrequency);
  return true;
}

bool CD3D12CommandSlot::Init(ID3D12Device3 * device,
  CD3D12CommandQueue * queue, const WCHAR * name, UINT queryBase,
  ULONG waitFlags)
{
  m_queue     = queue;
  m_name      = name;
  m_queryBase = queryBase;

  HRESULT hr = device->CreateCommandAllocator(
    queue->m_queue->GetDesc().Type, IID_PPV_ARGS(&m_allocator));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create the CommandAllocator (%ls)", name);
    return false;
  }

  hr = device->CreateCommandList(0, queue->m_queue->GetDesc().Type,
    m_allocator.Get(), NULL, IID_PPV_ARGS(&m_gfxList));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create the Graphics CommandList (%ls)", name);
    return false;
  }
  m_gfxList->SetName(name);

  m_cmdList = m_gfxList;
  if (!m_cmdList)
  {
    DEBUG_ERROR("Failed to get the CommandList (%ls)", name);
    return false;
  }

  m_event.Attach(CreateEvent(NULL, FALSE, FALSE, NULL));
  if (!m_event.Get())
  {
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to create the completion event (%ls)", name);
    return false;
  }

  m_availableEvent.Attach(CreateEvent(NULL, FALSE, FALSE, NULL));
  if (!m_availableEvent.Get())
  {
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to create the availability event (%ls)", name);
    return false;
  }

  if (!RegisterWaitForSingleObject(
    &m_waitHandle,
    m_event.Get(),
    [](PVOID param, BOOLEAN timeout){
      static_cast<CD3D12CommandSlot *>(param)->OnCompletion(!!timeout);
    },
    this,
    INFINITE,
    waitFlags))
  {
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to register the completion wait (%ls)", name);
    m_waitHandle = INVALID_HANDLE_VALUE;
    return false;
  }

  DEBUG_INFO("Created CD3D12CommandSlot(%ls)", name);
  return true;
}

void CD3D12CommandSlot::DeInit()
{
  if (m_waitHandle != INVALID_HANDLE_VALUE)
  {
    if (!UnregisterWaitEx(m_waitHandle, INVALID_HANDLE_VALUE))
      DEBUG_WARN_HR(GetLastError(),
        "Failed to unregister the completion wait (%ls)", m_name);
    m_waitHandle = INVALID_HANDLE_VALUE;
  }

  m_event.Close();
  m_availableEvent.Close();
  m_cmdList.Reset();
  m_gfxList.Reset();
  m_allocator.Reset();
  m_queue = nullptr;
}

bool CD3D12CommandSlot::Acquire()
{
  if (!m_queue || m_queue->m_failed.load(std::memory_order_acquire))
    return false;

  State expected = STATE_FREE;
  if (!m_state.compare_exchange_strong(expected, STATE_RECORDING,
      std::memory_order_acq_rel))
    return false;

  m_completionCallback  = nullptr;
  m_completionParams[0] = nullptr;
  m_completionParams[1] = nullptr;
  m_completionResult    = true;
  m_fenceTarget         = 0;
  m_fenceWaitCount      = 0;
  m_timingActive        = false;
  m_timestampFrequency  = 0;
  m_calibrationGPU      = 0;
  m_calibrationCPU      = 0;
  m_submitted.store(false, std::memory_order_release);

  for (UINT i = 0; i < MAX_FENCE_WAITS; ++i)
  {
    m_fenceWaits[i].fence.Reset();
    m_fenceWaits[i].value = 0;
  }

  if (!m_needsReset)
    return true;

  HRESULT hr = m_allocator->Reset();
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to reset the CommandAllocator (%ls)", m_name);
    m_queue->m_failed.store(true, std::memory_order_release);
    m_state.store(STATE_FAILED, std::memory_order_release);
    return false;
  }

  hr = m_gfxList->Reset(m_allocator.Get(), NULL);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to reset the CommandList (%ls)", m_name);
    m_queue->m_failed.store(true, std::memory_order_release);
    m_state.store(STATE_FAILED, std::memory_order_release);
    return false;
  }

  m_needsReset = false;
  return true;
}

void CD3D12CommandSlot::Cancel()
{
  State expected = STATE_RECORDING;
  if (!m_state.compare_exchange_strong(expected, STATE_CANCELLING,
      std::memory_order_acq_rel))
  {
    DEBUG_ERROR("Command slot cancelled while not recording (%ls)", m_name);
    return;
  }

  const HRESULT hr = m_gfxList->Close();
  m_needsReset   = true;
  m_timingActive = false;

  for (UINT i = 0; i < m_fenceWaitCount; ++i)
  {
    m_fenceWaits[i].fence.Reset();
    m_fenceWaits[i].value = 0;
  }
  m_fenceWaitCount = 0;

  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to close the cancelled CommandList (%ls)",
      m_name);
    m_queue->m_failed.store(true, std::memory_order_release);
    m_state.store(STATE_FAILED, std::memory_order_release);
    return;
  }

  m_completionCallback  = nullptr;
  m_completionParams[0] = nullptr;
  m_completionParams[1] = nullptr;
  m_submitted.store(false, std::memory_order_release);
  m_state.store(STATE_FREE, std::memory_order_release);
  SetEvent(m_availableEvent.Get());
}

bool CD3D12CommandSlot::Execute()
{
  State expected = STATE_RECORDING;
  if (!m_state.compare_exchange_strong(expected, STATE_SUBMITTED,
      std::memory_order_acq_rel))
  {
    DEBUG_ERROR("Command slot executed while not recording (%ls)", m_name);
    return false;
  }

  m_needsReset = true;
  HRESULT hr = m_gfxList->Close();
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to close the CommandList (%ls)", m_name);
    m_queue->m_failed.store(true, std::memory_order_release);
    m_state.store(STATE_FAILED, std::memory_order_release);
    return false;
  }

  if (m_queue->Submit(*this))
    return true;

  if (!m_submitted.load(std::memory_order_acquire))
  {
    m_state.store(STATE_FREE, std::memory_order_release);
    SetEvent(m_availableEvent.Get());
  }
  return false;
}

bool CD3D12CommandSlot::WaitFor(ID3D12Fence * fence, UINT64 value)
{
  if (!fence || !value ||
      m_state.load(std::memory_order_acquire) != STATE_RECORDING)
    return false;

  if (m_fenceWaitCount == MAX_FENCE_WAITS)
  {
    DEBUG_ERROR("Too many fence waits for CommandSlot(%ls)", m_name);
    return false;
  }

  FenceWait& wait = m_fenceWaits[m_fenceWaitCount++];
  wait.fence = fence;
  wait.value = value;
  return true;
}

bool CD3D12CommandSlot::WaitFor(const CD3D12CommandSlot& slot)
{
  if (!slot.m_queue || !slot.m_fenceTarget)
    return false;

  return WaitFor(slot.m_queue->m_fence.Get(), slot.m_fenceTarget);
}

bool CD3D12CommandSlot::BeginTiming()
{
  m_timingActive = m_queue && m_queue->m_timingSupported;
  if (!m_timingActive)
    return false;

  m_gfxList->EndQuery(m_queue->m_timestampHeap.Get(),
    D3D12_QUERY_TYPE_TIMESTAMP, m_queryBase);
  return true;
}

void CD3D12CommandSlot::EndTiming()
{
  if (!m_timingActive)
    return;

  if (!m_queue->SnapshotTiming(*this))
  {
    m_timingActive = false;
    return;
  }

  m_gfxList->EndQuery(m_queue->m_timestampHeap.Get(),
    D3D12_QUERY_TYPE_TIMESTAMP, m_queryBase + 1);
  m_gfxList->ResolveQueryData(m_queue->m_timestampHeap.Get(),
    D3D12_QUERY_TYPE_TIMESTAMP, m_queryBase, 2,
    m_queue->m_timestampReadback.Get(),
    (UINT64)m_queryBase * sizeof(UINT64));
}

bool CD3D12CommandSlot::GetGPUTimes(
  uint64_t& start, uint64_t& end) const
{
  return m_queue && m_queue->GetGPUTimes(*this, start, end);
}

void CD3D12CommandSlot::OnCompletion(bool timeout)
{
  if (!m_queue || !m_submitted.load(std::memory_order_acquire))
    return;

  const UINT64 completed = m_queue->m_fence->GetCompletedValue();
  if (completed != UINT64_MAX && completed < m_fenceTarget)
    return;

  State expected = STATE_SUBMITTED;
  if (!m_state.compare_exchange_strong(expected, STATE_COMPLETING,
      std::memory_order_acq_rel))
    return;

  m_completionResult = !timeout && completed != UINT64_MAX;
  if (!m_completionResult)
    m_queue->m_failed.store(true, std::memory_order_release);

  if (m_completionCallback)
    m_completionCallback(this, m_completionResult,
      m_completionParams[0], m_completionParams[1]);

  m_completionCallback  = nullptr;
  m_completionParams[0] = nullptr;
  m_completionParams[1] = nullptr;
  m_submitted.store(false, std::memory_order_release);
  m_state.store(STATE_FREE, std::memory_order_release);
  SetEvent(m_availableEvent.Get());
}

bool CD3D12CommandQueue::InitTiming(ID3D12Device3 * device, UINT slotCount)
{
  D3D12_FEATURE_DATA_D3D12_OPTIONS3 options = {};
  HRESULT hr = device->CheckFeatureSupport(
    D3D12_FEATURE_D3D12_OPTIONS3, &options, sizeof(options));
  if (FAILED(hr) || !options.CopyQueueTimestampQueriesSupported)
    return false;

  LARGE_INTEGER qpcFrequency;
  if (!QueryPerformanceFrequency(&qpcFrequency))
    return false;
  m_qpcFrequency = (UINT64)qpcFrequency.QuadPart;

  D3D12_QUERY_HEAP_DESC queryDesc = {};
  queryDesc.Type  = D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP;
  queryDesc.Count = slotCount * 2;

  hr = device->CreateQueryHeap(&queryDesc,
    IID_PPV_ARGS(&m_timestampHeap));
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
  resourceDesc.Width              = sizeof(UINT64) * queryDesc.Count;
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

  const SIZE_T timingSize  = sizeof(UINT64) * queryDesc.Count;
  D3D12_RANGE readRange    = { 0, timingSize };
  void      * timestampMap = nullptr;
  hr = m_timestampReadback->Map(0, &readRange, &timestampMap);
  if (FAILED(hr))
  {
    m_timestampReadback.Reset();
    m_timestampHeap.Reset();
    return false;
  }

  m_timestampMap    = static_cast<UINT64 *>(timestampMap);
  m_timingSupported = true;
  return true;
}

bool CD3D12CommandQueue::Init(ID3D12Device3 * device,
  D3D12_COMMAND_LIST_TYPE type, const WCHAR * name,
  CD3D12CommandSlot::CallbackMode callbackMode, UINT slotCount,
  bool enableTiming)
{
  if (!slotCount || slotCount > MAX_SLOTS)
  {
    DEBUG_ERROR("Invalid slot count for CommandQueue(%ls): %u",
      name, slotCount);
    return false;
  }

  D3D12_COMMAND_QUEUE_DESC queueDesc = {};
  queueDesc.Type     = type;
  queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
  queueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;

  HRESULT hr = device->CreateCommandQueue(
    &queueDesc, IID_PPV_ARGS(&m_queue));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create the CommandQueue (%ls)", name);
    return false;
  }
  m_queue->SetName(name);

  hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
    IID_PPV_ARGS(&m_fence));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create the CommandQueue fence (%ls)", name);
    return false;
  }

  if (enableTiming && !InitTiming(device, slotCount))
    DEBUG_WARN("GPU timing is unavailable for CommandQueue(%ls)", name);

  const ULONG waitFlags = callbackMode == CD3D12CommandSlot::FAST ?
    WT_EXECUTEINWAITTHREAD : WT_EXECUTEINPERSISTENTTHREAD;

  m_name = name;
  for (UINT i = 0; i < slotCount; ++i)
  {
    if (!m_slots[i].Init(device, this, name, i * 2, waitFlags))
      return false;
    ++m_slotCount;
  }

  DEBUG_INFO("Created CD3D12CommandQueue(%ls) with %u slots",
    name, slotCount);
  return true;
}

void CD3D12CommandQueue::DeInit()
{
  WaitForIdle();

  for (UINT i = 0; i < MAX_SLOTS; ++i)
    m_slots[i].DeInit();
  m_slotCount = 0;

  if (m_timestampMap)
  {
    D3D12_RANGE writeRange = { 0, 0 };
    m_timestampReadback->Unmap(0, &writeRange);
    m_timestampMap = nullptr;
  }

  m_timestampReadback.Reset();
  m_timestampHeap.Reset();
  m_fence.Reset();
  m_queue.Reset();
  m_timingSupported = false;
  m_qpcFrequency    = 0;
}

CD3D12CommandSlot * CD3D12CommandQueue::Acquire(UINT slotIndex)
{
  if (slotIndex >= m_slotCount)
    return nullptr;

  const ULONGLONG deadline = GetTickCount64() + 100;
  for (;;)
  {
    if (m_slots[slotIndex].Acquire())
      return &m_slots[slotIndex];

    // The GPU may already be finished while its thread-pool callback is
    // still pending. Complete it here before sleeping on callback dispatch.
    m_slots[slotIndex].OnCompletion(false);
    if (m_slots[slotIndex].Acquire())
      return &m_slots[slotIndex];
    if (m_failed.load(std::memory_order_acquire))
      break;

    const ULONGLONG now = GetTickCount64();
    if (now >= deadline)
      break;

    const DWORD result =
      WaitForSingleObject(m_slots[slotIndex].m_availableEvent.Get(),
        static_cast<DWORD>(deadline - now));
    if (result != WAIT_OBJECT_0)
      break;
  }

  DEBUG_ERROR("Failed to acquire CommandSlot(%ls:%u)", m_name, slotIndex);
  return nullptr;
}

CD3D12CommandSlot * CD3D12CommandQueue::Acquire()
{
  if (!m_slotCount)
    return nullptr;

  // The unindexed path is used for immediate software publication. Keep at
  // most one copy in flight so a newer frame is dropped instead of queued
  // behind bandwidth-bound work which is already stale.
  CD3D12CommandSlot& slot = m_slots[0];
  if (slot.Acquire())
    return &slot;

  // Complete already-fenced work without waiting for callback dispatch.
  slot.OnCompletion(false);
  if (slot.Acquire())
    return &slot;

  return nullptr;
}

void CD3D12CommandQueue::WaitForIdle()
{
  for (UINT slot = 0; slot < m_slotCount; ++slot)
  {
    while (!m_slots[slot].IsIdle())
    {
      // The callback may be delayed behind other thread-pool work. If its
      // fence has completed, finish it here before releasing callback state.
      m_slots[slot].OnCompletion(false);
      if (m_slots[slot].IsIdle())
        break;
      Sleep(1);
    }
  }
}

bool CD3D12CommandQueue::Submit(CD3D12CommandSlot& slot)
{
  bool result = false;
  CSRWExclusiveLock lock(m_submitLock);

  do
  {
    if (m_failed.load(std::memory_order_relaxed))
      break;

    for (UINT i = 0; i < slot.m_fenceWaitCount; ++i)
    {
      const CD3D12CommandSlot::FenceWait& wait = slot.m_fenceWaits[i];
      const HRESULT hr = m_queue->Wait(wait.fence.Get(), wait.value);
      if (FAILED(hr))
      {
        DEBUG_ERROR_HR(hr, "Failed to queue a fence wait (%ls)", m_name);
        m_failed.store(true, std::memory_order_release);
        break;
      }
    }
    if (m_failed.load(std::memory_order_relaxed))
      break;

    const UINT64 fenceTarget = ++m_fenceValue;
    slot.m_fenceTarget       = fenceTarget;
    slot.m_submitted.store(true, std::memory_order_release);

    ID3D12CommandList * lists[] = { slot.m_cmdList.Get() };
    m_queue->ExecuteCommandLists(1, lists);

    HRESULT hr = m_queue->Signal(m_fence.Get(), fenceTarget);
    if (FAILED(hr))
    {
      DEBUG_ERROR_HR(hr, "Failed to signal the CommandQueue (%ls)", m_name);
      m_failed.store(true, std::memory_order_release);
      slot.OnCompletion(false);
      break;
    }

    hr = m_fence->SetEventOnCompletion(fenceTarget, slot.m_event.Get());
    if (FAILED(hr))
    {
      DEBUG_ERROR_HR(hr,
        "Failed to register CommandSlot completion (%ls)", m_name);

      // The work is already submitted and fenced. Poll only on this rare
      // error path so allocator, callback, and framebuffer ownership remain
      // valid until completion or confirmed device removal.
      while (slot.m_submitted.load(std::memory_order_acquire))
      {
        slot.OnCompletion(false);
        if (slot.m_submitted.load(std::memory_order_acquire))
          Sleep(1);
      }

      result = slot.m_completionResult;
      break;
    }

    result = true;
  }
  while (false);

  return result;
}

bool CD3D12CommandQueue::SnapshotTiming(
  CD3D12CommandSlot& slot) const
{
  UINT64 frequency;
  UINT64 gpu;
  UINT64 cpu;
  if (FAILED(m_queue->GetTimestampFrequency(&frequency)) || !frequency ||
      FAILED(m_queue->GetClockCalibration(&gpu, &cpu)))
    return false;

  slot.m_timestampFrequency = frequency;
  slot.m_calibrationGPU     = gpu;
  slot.m_calibrationCPU     = cpu;
  return true;
}

bool CD3D12CommandQueue::GetGPUTimes(const CD3D12CommandSlot& slot,
  uint64_t& start, uint64_t& end) const
{
  if (!slot.m_timingActive || !m_timestampMap ||
      !slot.m_timestampFrequency || !m_qpcFrequency)
    return false;

  const UINT64 gpuStart = m_timestampMap[slot.m_queryBase];
  const UINT64 gpuEnd   = m_timestampMap[slot.m_queryBase + 1];
  if (gpuEnd < gpuStart ||
      !ConvertGPUTimestamp(gpuStart, slot.m_timestampFrequency,
        slot.m_calibrationGPU, slot.m_calibrationCPU,
        m_qpcFrequency, start) ||
      !ConvertGPUTimestamp(gpuEnd, slot.m_timestampFrequency,
        slot.m_calibrationGPU, slot.m_calibrationCPU,
        m_qpcFrequency, end) ||
      end < start)
    return false;

  return true;
}
