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

#include "d3d/CInteropPool.h"

#include "CDebug.h"

#include <new>

using Microsoft::WRL::Wrappers::HandleT;
using Microsoft::WRL::Wrappers::HandleTraits::HANDLENullTraits;

static bool SameObject(IUnknown * left, IUnknown * right)
{
  if (!left || !right)
    return false;
  ComPtr<IUnknown> leftId;
  ComPtr<IUnknown> rightId;
  return
    SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&leftId)))   &&
    SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&rightId))) &&
    leftId.Get() == rightId.Get();
}

static bool SameDesc(const D3D12_RESOURCE_DESC& d12,
  const D3D11_TEXTURE2D_DESC& d11)
{
  return
    d12.Dimension          == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
    d12.Width              == d11.Width                          &&
    d12.Height             == d11.Height                         &&
    d12.DepthOrArraySize   == d11.ArraySize                      &&
    d12.MipLevels          == d11.MipLevels                      &&
    d12.Format             == d11.Format                         &&
    d12.SampleDesc.Count   == d11.SampleDesc.Count               &&
    d12.SampleDesc.Quality == d11.SampleDesc.Quality             &&
    d11.Usage              == D3D11_USAGE_DEFAULT                &&
    d11.CPUAccessFlags     == 0;
}

D12SyncState D11Sync::State() const
{
  if (ready)
    return D12SyncState::READY;
  if (!Valid())
    return D12SyncState::FAILED;
  const UINT64 completed = fence->GetCompletedValue();
  if (completed == UINT64_MAX)
    return D12SyncState::FAILED;
  return completed >= value ?
    D12SyncState::READY : D12SyncState::PENDING;
}

struct CD3D11Core
{
  static const unsigned MAX_FENCES = 4;

  struct View
  {
    uint64_t                 pool = 0;
    ComPtr<ID3D12Resource>   src;
    ComPtr<ID3D11Texture2D>  tex;
  };

  struct Fence
  {
    ComPtr<ID3D12Fence> src;
    ComPtr<ID3D11Fence> fence;
  };

  mutable CSRWLock              lock;
  bool                          open   = true;
  bool                          failed = false;
  uint64_t                      pool   = 0;
  UINT                          bind   = 0;
  std::shared_ptr<CD3D11Device> d11;
  std::shared_ptr<CD3D12Device> d12;
  ComPtr<ID3D11Device5>         dev11;
  ComPtr<ID3D12Device3>         dev12;
  View                          views[CTexPool::MAX_SLOTS];
  Fence                         fences[MAX_FENCES];

  void Stop()
  {
    CSRWExclusiveLock guard(lock);
    open = false;
  }

  void Fail()
  {
    CSRWExclusiveLock guard(lock);
    failed = true;
  }

  TexResult Error(HRESULT hr)
  {
    const HRESULT removed   = dev12->GetDeviceRemovedReason();
    const HRESULT removed11 = dev11->GetDeviceRemovedReason();
    if (removed != S_OK || removed11 != S_OK || hr == E_OUTOFMEMORY)
    {
      const HRESULT reason = removed != S_OK ? removed :
        (removed11 != S_OK ? removed11 : hr);
      DEBUG_ERROR_HR(reason, "D3D11 texture interop failed");
      failed = true;
      return TexResult::FAILED;
    }
    DEBUG_WARN_HR(hr, "D3D11 texture interop is unavailable");
    return TexResult::REJECTED;
  }

  TexResult OpenView(const CFrameTex& frame,
    ComPtr<ID3D11Texture2D>& texture)
  {
    if (frame.slot >= CTexPool::MAX_SLOTS)
      return TexResult::REJECTED;

    View& view = views[frame.slot];
    if (view.tex)
    {
      if (view.pool != frame.pool ||
          !SameObject(view.src.Get(), frame.Get()))
      {
        failed = true;
        return TexResult::FAILED;
      }
      texture = view.tex;
      return TexResult::OK;
    }

    HANDLE raw = nullptr;
    const HRESULT create = dev12->CreateSharedHandle(frame.Get(), nullptr,
      GENERIC_ALL, nullptr, &raw);
    if (FAILED(create))
      return Error(create);
    HandleT<HANDLENullTraits> handle;
    handle.Attach(raw);

    ComPtr<ID3D11Texture2D> opened;
    const HRESULT hr = dev11->OpenSharedResource1(handle.Get(),
      IID_PPV_ARGS(&opened));
    handle.Close();
    if (FAILED(hr))
      return Error(hr);

    D3D11_TEXTURE2D_DESC desc11 = {};
    opened->GetDesc(&desc11);
    if (!SameDesc(frame.Get()->GetDesc(), desc11) ||
        (desc11.BindFlags & bind) != bind)
      return TexResult::REJECTED;

    view.pool  = frame.pool;
    view.src   = frame.Get();
    view.tex   = opened;
    texture    = opened;
    return TexResult::OK;
  }

  TexResult OpenFence(const D12Sync& sync, D11Sync& result)
  {
    ComPtr<ID3D12Device> owner;
    if (!sync.Valid() ||
        FAILED(sync.fence->GetDevice(IID_PPV_ARGS(&owner))) ||
        !SameObject(owner.Get(), dev12.Get()))
      return TexResult::REJECTED;

    const D12SyncState state = sync.State();
    if (state == D12SyncState::FAILED)
    {
      failed = true;
      return TexResult::FAILED;
    }
    if (state == D12SyncState::READY)
    {
      result.ready = true;
      return TexResult::OK;
    }

    for (unsigned i = 0; i < MAX_FENCES; ++i)
      if (fences[i].fence &&
          SameObject(fences[i].src.Get(), sync.fence.Get()))
      {
        result.fence = fences[i].fence;
        result.value = sync.value;
        return TexResult::OK;
      }

    unsigned free = MAX_FENCES;
    for (unsigned i = 0; i < MAX_FENCES; ++i)
      if (!fences[i].fence)
      {
        free = i;
        break;
      }
    if (free == MAX_FENCES)
      return TexResult::BUSY;

    HANDLE raw = nullptr;
    const HRESULT create = dev12->CreateSharedHandle(sync.fence.Get(),
      nullptr, GENERIC_ALL, nullptr, &raw);
    if (FAILED(create))
      return Error(create);
    HandleT<HANDLENullTraits> handle;
    handle.Attach(raw);

    ComPtr<ID3D11Fence> opened;
    const HRESULT hr = dev11->OpenSharedFence(handle.Get(),
      IID_PPV_ARGS(&opened));
    handle.Close();
    if (FAILED(hr))
      return Error(hr);

    fences[free].src   = sync.fence;
    fences[free].fence = opened;
    result.fence       = opened;
    result.value       = sync.value;
    return TexResult::OK;
  }

  TexResult Get(const TexLease& src, D11Lease& lease)
  {
    const CFrameTex * frame = src.Get();
    if (!frame                                                       ||
        frame->profile.storage != FrameStorage::D3D12_TEXTURE        ||
        !frame->Shared()                                             ||
        frame->State()          != D3D12_RESOURCE_STATE_COMMON       ||
        !frame->Get()                                                ||
        !frame->Sync().Valid())
      return TexResult::REJECTED;

    ComPtr<ID3D12Device> owner;
    if (FAILED(frame->Get()->GetDevice(IID_PPV_ARGS(&owner))) ||
        !SameObject(owner.Get(), dev12.Get()))
      return TexResult::REJECTED;

    CSRWExclusiveLock guard(lock);
    if (!open || failed)
      return TexResult::FAILED;
    if (frame->Status() == D12SyncState::FAILED)
    {
      failed = true;
      return TexResult::FAILED;
    }
    if (!pool)
      pool = frame->pool;
    else if (pool != frame->pool)
      return TexResult::REJECTED;

    ComPtr<ID3D11Texture2D> texture;
    TexResult result = OpenView(*frame, texture);
    if (result != TexResult::OK)
      return result;

    D11Sync sync;
    result = OpenFence(frame->Sync(), sync);
    if (result != TexResult::OK)
      return result;

    const FrameProfile profile =
      Frame::Store(frame->profile, FrameStorage::D3D11_TEXTURE);
    lease = D11Lease(shared_from_this(), src, texture, sync, profile);
    return TexResult::OK;
  }

private:
  // Filled by Init after construction; avoids exposing ownership callbacks
  // from cached COM objects.
  std::weak_ptr<CD3D11Core> self;

  std::shared_ptr<CD3D11Core> shared_from_this()
  {
    return self.lock();
  }

  friend class CInteropPool;
};

D11Lease::D11Lease(const std::shared_ptr<CD3D11Core>& core,
  const TexLease& src, const ComPtr<ID3D11Texture2D>& tex,
  const D11Sync& sync, const FrameProfile& profile) :
  m_core(core),
  m_src(src),
  m_tex(tex),
  m_sync(sync),
  m_profile(profile)
{
}

TexResult D11Lease::Wait(ID3D11DeviceContext4 * context) const
{
  if (!m_core || !m_src || !m_tex || !context || !m_sync.Valid())
    return TexResult::REJECTED;

  ComPtr<ID3D11Device> owner;
  context->GetDevice(owner.GetAddressOf());
  if (!SameObject(owner.Get(), m_core->dev11.Get()))
    return TexResult::REJECTED;

  const D12SyncState state = m_sync.State();
  if (state == D12SyncState::FAILED)
  {
    m_core->Fail();
    return TexResult::FAILED;
  }
  if (state == D12SyncState::READY)
    return TexResult::OK;

  const HRESULT hr = context->Wait(m_sync.fence.Get(), m_sync.value);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to queue the D3D11 producer wait");
    m_core->Fail();
    return TexResult::FAILED;
  }
  return TexResult::OK;
}

void D11Lease::Reset()
{
  m_sync = D11Sync {};
  m_tex.Reset();
  m_src.Reset();
  m_core.reset();
  m_profile = FrameProfile {};
}

TexResult CInteropPool::Init(
  const std::shared_ptr<CD3D11Device>& d11,
  const std::shared_ptr<CD3D12Device>& d12, UINT bind)
{
  if (!d11 || !d12 || !Frame::Same(d11->GetAdapterLuid(),
      d12->GetAdapterLuid()))
    return TexResult::REJECTED;

  ComPtr<ID3D11Device5> dev11 = d11->GetDevice();
  ComPtr<ID3D12Device3> dev12 = d12->GetDevice();
  if (!dev11 || !dev12)
    return TexResult::REJECTED;

  std::shared_ptr<CD3D11Core> core;
  try
  {
    core = std::shared_ptr<CD3D11Core>(new (std::nothrow) CD3D11Core);
  }
  catch (const std::bad_alloc&)
  {
    return TexResult::FAILED;
  }
  if (!core)
    return TexResult::FAILED;
  core->d11   = d11;
  core->d12   = d12;
  core->dev11 = dev11;
  core->dev12 = dev12;
  core->bind  = bind;
  core->self  = core;

  std::shared_ptr<CD3D11Core> old;
  {
    CSRWExclusiveLock guard(m_lock);
    old = std::move(m_core);
    if (old)
      old->Stop();
    m_core = core;
  }
  return TexResult::OK;
}

TexResult CInteropPool::Get(const TexLease& src, D11Lease& lease)
{
  if (lease)
    return TexResult::REJECTED;

  std::shared_ptr<CD3D11Core> core;
  {
    CSRWSharedLock guard(m_lock);
    core = m_core;
  }
  return core ? core->Get(src, lease) : TexResult::FAILED;
}

void CInteropPool::Reset()
{
  std::shared_ptr<CD3D11Core> core;
  {
    CSRWExclusiveLock guard(m_lock);
    core = std::move(m_core);
    if (core)
      core->Stop();
  }
}
