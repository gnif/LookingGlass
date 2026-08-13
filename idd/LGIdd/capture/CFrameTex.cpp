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

#include "capture/CFrameTex.h"

#include "Atomic.h"
#include "CDebug.h"
#include "CSRWLock.h"
#include "Seq.h"

#include <new>

enum class TexState : uint8_t
{
  FREE,
  WRITE,
  LEASED,
  WAIT,
  DEAD,
};

static void WaitSync(const D12Sync& sync)
{
  while (sync.State() == D12SyncState::PENDING)
    Sleep(1);
}

static uint64_t NextPool()
{
  static std::atomic<uint64_t> next { 0 };
  return Atomic::Next(next);
}

static bool ValidFrame(const FrameProfile& profile,
  const D3D12_RESOURCE_DESC& resource, const FrameDesc& frame)
{
  const D12FrameFormat& format = frame.format;
  FrameProfile formatProfile;
  const bool validProfile = D12::Profile(format,
    FrameStorage::D3D12_TEXTURE, formatProfile);
  if (!validProfile || !Frame::Same(formatProfile, profile))
    return false;

  const bool validDamage = Frame::Valid(frame.damage, frame.rects,
    frame.count, format.width, format.height);
  return
    frame.content                                                   &&
    frame.content->serial                                           &&
    validDamage                                                     &&
    resource.Dimension        == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
    resource.Width            == format.width                       &&
    resource.Height           == format.height                      &&
    resource.DepthOrArraySize == 1                                  &&
    resource.MipLevels        == 1                                  &&
    resource.SampleDesc.Count == 1                                  &&
    D12::Same(resource, format.desc, D12::DescCmp::NO_FLAGS)        &&
    format.pitch              == 0                                  &&
    format.dataWidth          == resource.Width                     &&
    format.dataHeight         == resource.Height;
}

struct CTexCore
{
  struct Slot
  {
    ComPtr<ID3D12Resource> res;
    D12Sync                sync;
    TexState               state = TexState::FREE;
    uint64_t               gen   = 0;
  };

  mutable CSRWLock       lock;
  bool                   open    = true;
  bool                   closed  = false;
  bool                   failed  = false;
  uint64_t               graph   = 0;
  uint64_t               id      = 0;
  unsigned               node    = 0;
  unsigned               count   = 0;
  FrameProfile           profile;
  D3D12_RESOURCE_DESC    desc    = {};
  D3D12_RESOURCE_STATES  read    = D3D12_RESOURCE_STATE_COMMON;
  Slot                   slots[CTexPool::MAX_SLOTS];

  TexResult Acquire(unsigned& index, uint64_t& generation)
  {
    CSRWExclusiveLock guard(lock);
    if (!open || failed)
      return TexResult::FAILED;

    for (unsigned i = 0; i < count; ++i)
      if (slots[i].state == TexState::WAIT)
      {
        const D12SyncState state = slots[i].sync.State();
        if (state == D12SyncState::FAILED)
        {
          slots[i].state = TexState::DEAD;
          failed         = true;
          return TexResult::FAILED;
        }
        else if (state == D12SyncState::READY)
        {
          slots[i].sync  = D12Sync {};
          slots[i].state = TexState::FREE;
        }
      }

    for (unsigned i = 0; i < count; ++i)
      if (slots[i].state == TexState::FREE)
      {
        Seq::Inc(slots[i].gen);
        slots[i].state = TexState::WRITE;
        index          = i;
        generation     = slots[i].gen;
        return TexResult::OK;
      }

    for (unsigned i = 0; i < count; ++i)
      if (slots[i].state != TexState::DEAD)
        return TexResult::BUSY;
    return TexResult::FAILED;
  }

  bool Info(unsigned index, uint64_t generation,
    ComPtr<ID3D12Resource>& resource) const
  {
    CSRWSharedLock guard(lock);
    if (!open || failed || index >= count ||
        slots[index].gen != generation ||
        slots[index].state != TexState::WRITE)
      return false;
    resource = slots[index].res;
    return true;
  }

  bool Seal(unsigned index, uint64_t generation, const D12Sync& sync)
  {
    CSRWExclusiveLock guard(lock);
    if (!open || failed || index >= count ||
        slots[index].gen != generation ||
        slots[index].state != TexState::WRITE || !sync.Valid())
      return false;
    slots[index].sync  = sync;
    slots[index].state = TexState::LEASED;
    return true;
  }

  void Fail(unsigned index, uint64_t generation, const D12Sync& sync)
  {
    D12Sync retired;
    {
      CSRWExclusiveLock guard(lock);
      if (index >= count || slots[index].gen != generation ||
          slots[index].state != TexState::WRITE)
        return;
      slots[index].sync = sync;
      if (open)
        slots[index].state = TexState::WAIT;
      else
      {
        retired = sync;
        slots[index].state = TexState::DEAD;
      }
    }
    if (retired.Valid())
      WaitSync(retired);
  }

  void Drop(unsigned index, uint64_t generation)
  {
    D12Sync retired;
    {
      CSRWExclusiveLock guard(lock);
      if (index >= count || slots[index].gen != generation)
        return;

      Slot& slot = slots[index];
      if (slot.state == TexState::WRITE)
      {
        slot.state = TexState::DEAD;
        return;
      }
      if (slot.state != TexState::LEASED)
        return;

      const D12SyncState state = slot.sync.State();
      if (!open)
      {
        retired    = slot.sync;
        slot.state = TexState::DEAD;
      }
      else if (state == D12SyncState::FAILED)
      {
        slot.state = TexState::DEAD;
        failed     = true;
      }
      else if (state == D12SyncState::READY)
      {
        slot.sync  = D12Sync {};
        slot.state = TexState::FREE;
      }
      else
        slot.state = TexState::WAIT;
    }
    if (retired.Valid())
      WaitSync(retired);
  }

  void Cancel(unsigned index, uint64_t generation)
  {
    CSRWExclusiveLock guard(lock);
    if (index >= count || slots[index].gen != generation ||
        slots[index].state != TexState::WRITE)
      return;
    slots[index].state = open ? TexState::FREE : TexState::DEAD;
  }

  void Retire(unsigned index, uint64_t generation)
  {
    CSRWExclusiveLock guard(lock);
    if (index < count && slots[index].gen == generation &&
        slots[index].state == TexState::WRITE)
      slots[index].state = TexState::DEAD;
  }

  void Close()
  {
    D12Sync waits[CTexPool::MAX_SLOTS];
    unsigned waitCount = 0;
    {
      CSRWExclusiveLock guard(lock);
      if (closed)
        return;
      open   = false;
      closed = true;
      for (unsigned i = 0; i < count; ++i)
      {
        if ((slots[i].state == TexState::LEASED ||
             slots[i].state == TexState::WAIT) &&
            slots[i].sync.Valid())
          waits[waitCount++] = slots[i].sync;
        if (slots[i].state != TexState::LEASED &&
            slots[i].state != TexState::WRITE)
          slots[i].state = TexState::DEAD;
      }
    }

    // Reset is infrequent and cannot release a texture still referenced by
    // producer commands. Outstanding consumers keep their own core alive.
    for (unsigned i = 0; i < waitCount; ++i)
      WaitSync(waits[i]);
  }

  void Stop()
  {
    CSRWExclusiveLock guard(lock);
    open = false;
  }
};

CFrameTex::CFrameTex(uint64_t graphValue, uint64_t poolValue,
  unsigned nodeValue, unsigned slotValue, uint64_t versionValue,
  const FrameProfile& profileValue,
  const FrameDesc& frameValue, const ComPtr<ID3D12Resource>& res,
  const D12Sync& sync, D3D12_RESOURCE_STATES state) :
  m_res(res),
  m_sync(sync),
  m_state(state),
  graph(graphValue),
  pool(poolValue),
  node(nodeValue),
  slot(slotValue),
  version(versionValue),
  profile(profileValue),
  frame(frameValue)
{
}

bool CFrameTex::Wait(CD3D12CommandSlot& cmd) const
{
  if (!m_sync.Valid())
    return false;
  const UINT64 completed = m_sync.fence->GetCompletedValue();
  if (completed == UINT64_MAX)
    return false;
  if (completed >= m_sync.value)
    return true;
  return cmd.WaitFor(m_sync.fence.Get(), m_sync.value);
}

TexWrite::TexWrite(const std::shared_ptr<CTexCore>& core,
  unsigned index, uint64_t version) :
  m_core(core), m_index(index), m_version(version)
{
}

TexWrite::TexWrite(TexWrite&& other) noexcept :
  m_core(std::move(other.m_core)),
  m_index(other.m_index),
  m_version(other.m_version)
{
  other.Clear();
}

TexWrite& TexWrite::operator=(TexWrite&& other) noexcept
{
  if (this == &other)
    return *this;
  Retire();
  m_core    = std::move(other.m_core);
  m_index   = other.m_index;
  m_version = other.m_version;
  other.Clear();
  return *this;
}

TexWrite::~TexWrite()
{
  Retire();
}

void TexWrite::Retire()
{
  if (m_core)
    m_core->Retire(m_index, m_version);
  Clear();
}

void TexWrite::Clear()
{
  m_core.reset();
  m_index   = 0;
  m_version = 0;
}

ID3D12Resource * TexWrite::Get() const
{
  ComPtr<ID3D12Resource> resource;
  return m_core && m_core->Info(m_index, m_version, resource) ?
    resource.Get() : nullptr;
}

bool TexWrite::Seal(const FrameDesc& frame, const D12Sync& sync,
  TexLease& lease)
{
  if (!m_core || !sync.Valid())
    return false;

  const std::shared_ptr<CTexCore> core = m_core;
  ComPtr<ID3D12Resource> resource;
  FrameDesc desc = frame;
  if (!core->Info(m_index, m_version, resource) ||
      !ValidFrame(core->profile, resource->GetDesc(), desc))
  {
    core->Fail(m_index, m_version, sync);
    Clear();
    return false;
  }

  // The graph descriptor is resource-free and canonical. Publish the exact
  // pool descriptor, including producer-only UAV creation capability.
  desc.format.desc = resource->GetDesc();

  const unsigned index = m_index;
  const uint64_t generation = m_version;
  if (!core->Seal(index, generation, sync))
  {
    core->Fail(index, generation, sync);
    Clear();
    return false;
  }

  CFrameTex * raw = new (std::nothrow) CFrameTex(core->graph, core->id,
    core->node, m_index, m_version, core->profile, desc, resource, sync,
    core->read);
  if (!raw)
  {
    core->Drop(index, generation);
    Clear();
    return false;
  }

  std::shared_ptr<const CFrameTex> product;
  try
  {
    product = std::shared_ptr<const CFrameTex>(raw,
      [core, index, generation](const CFrameTex * texture)
      {
        delete texture;
        core->Drop(index, generation);
      });
  }
  catch (const std::bad_alloc&)
  {
    // The shared_ptr constructor invokes the supplied deleter on failure.
    Clear();
    return false;
  }

  Clear();
  lease = TexLease(product);
  return true;
}

void TexWrite::Cancel()
{
  if (m_core)
    m_core->Cancel(m_index, m_version);
  Clear();
}

TexResult CTexPool::Init(ID3D12Device3 * device, uint64_t graph,
  unsigned node, const FrameProfile& profile,
  const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state,
  unsigned slots)
{
  if (!device || !graph || !node || !slots || slots > MAX_SLOTS     ||
      profile.storage         != FrameStorage::D3D12_TEXTURE        ||
      !Frame::Valid(profile)                                        ||
      desc.Dimension          != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      !desc.Width || !desc.Height || desc.Width > UINT_MAX          ||
      desc.Format             != D12::Dxgi(profile.pixel)           ||
      desc.DepthOrArraySize   != 1                                  ||
      desc.MipLevels          != 1                                  ||
      desc.SampleDesc.Count   != 1                                  ||
      desc.SampleDesc.Quality != 0                                  ||
      desc.Alignment          != 0                                  ||
      desc.Layout             != D3D12_TEXTURE_LAYOUT_UNKNOWN       ||
      (static_cast<UINT>(desc.Flags) &
       ~static_cast<UINT>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
         D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)) != 0       ||
      node                    >= FRAME_GRAPH_MAX_NODES              ||
      state                   != D3D12_RESOURCE_STATE_COMMON)
    return TexResult::REJECTED;

  std::shared_ptr<CTexCore> core;
  try
  {
    core = std::shared_ptr<CTexCore>(new (std::nothrow) CTexCore);
  }
  catch (const std::bad_alloc&)
  {
    return TexResult::FAILED;
  }
  if (!core)
    return TexResult::FAILED;
  core->graph   = graph;
  core->id      = NextPool();
  core->node    = node;
  core->count   = slots;
  core->profile = profile;
  core->desc    = desc;
  core->read    = state;

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type                 = D3D12_HEAP_TYPE_DEFAULT;
  heap.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  heap.CreationNodeMask     = 1;
  heap.VisibleNodeMask      = 1;

  for (unsigned i = 0; i < slots; ++i)
  {
    const HRESULT hr = device->CreateCommittedResource(&heap,
      D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &desc, state, nullptr,
      IID_PPV_ARGS(&core->slots[i].res));
    if (FAILED(hr))
    {
      DEBUG_ERROR_HR(hr, "Failed to create frame texture");
      core->Close();
      return TexResult::FAILED;
    }
    core->slots[i].res->SetName(L"Frame Texture");
  }

  std::shared_ptr<CTexCore> old;
  {
    CSRWExclusiveLock guard(m_lock);
    old = std::move(m_core);
    if (old)
      old->Stop();
    m_core = core;
  }
  if (old)
    old->Close();
  return TexResult::OK;
}

void CTexPool::Reset()
{
  std::shared_ptr<CTexCore> core;
  {
    CSRWExclusiveLock guard(m_lock);
    core = std::move(m_core);
    if (core)
      core->Stop();
  }
  if (core)
    core->Close();
}

TexResult CTexPool::Try(TexWrite& write)
{
  if (write)
    return TexResult::REJECTED;

  std::shared_ptr<CTexCore> core;
  {
    CSRWSharedLock guard(m_lock);
    core = m_core;
  }
  if (!core)
    return TexResult::FAILED;
  unsigned index;
  uint64_t generation;
  const TexResult result = core->Acquire(index, generation);
  if (result == TexResult::OK)
    write = TexWrite(core, index, generation);
  return result;
}
