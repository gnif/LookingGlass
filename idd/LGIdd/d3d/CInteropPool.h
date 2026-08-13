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

#pragma once

#include "CSRWLock.h"
#include "capture/CFrameTex.h"
#include "d3d/CD3D11Device.h"
#include "d3d/CD3D12Device.h"

#include <memory>

struct CD3D11Core;

struct D11Sync
{
  ComPtr<ID3D11Fence> fence;
  UINT64              value = 0;
  bool                ready = false;

  bool Valid() const { return ready || (fence && value); }
  D12SyncState State() const;
};

// A native D3D11 view of one immutable graph product. Keep the lease until
// the encoder's GPU completion, not merely until its submission returns.
class D11Lease
{
  friend class CInteropPool;
  friend struct CD3D11Core;

private:
  std::shared_ptr<CD3D11Core> m_core;
  TexLease                    m_src;
  ComPtr<ID3D11Texture2D>     m_tex;
  D11Sync                     m_sync;
  FrameProfile                m_profile;

  D11Lease(const std::shared_ptr<CD3D11Core>& core, const TexLease& src,
    const ComPtr<ID3D11Texture2D>& tex, const D11Sync& sync,
    const FrameProfile& profile);

public:
  D11Lease() = default;
  D11Lease(const D11Lease&) = default;
  D11Lease& operator=(const D11Lease&) = default;
  D11Lease(D11Lease&&) noexcept = default;
  D11Lease& operator=(D11Lease&&) noexcept = default;

  explicit operator bool() const { return !!m_src && !!m_tex; }
  ID3D11Texture2D * Get() const { return m_tex.Get(); }
  const CFrameTex * Frame() const { return m_src.Get(); }
  const FrameProfile& Profile() const { return m_profile; }
  const D11Sync& Sync() const { return m_sync; }
  D12SyncState Status() const { return m_sync.State(); }

  // Orders only work subsequently submitted through this exact context.
  // The context owner remains responsible for its thread serialization and
  // for ordering any private encoder queue.
  TexResult Wait(ID3D11DeviceContext4 * context) const;
  void Reset();
};

// Opens cached native D3D11 views of shareable D3D12 graph products. The
// pool never copies, transitions, or writes a product. Failures remain local
// to this interop route and cannot invalidate the source or sibling routes.
class CInteropPool
{
private:
  mutable CSRWLock            m_lock;
  std::shared_ptr<CD3D11Core> m_core;

public:
  CInteropPool() = default;
  CInteropPool(const CInteropPool&) = delete;
  CInteropPool& operator=(const CInteropPool&) = delete;
  ~CInteropPool() { Reset(); }

  // bind is the minimum native D3D11 bind capability required by the future
  // consumer. Unsupported capabilities reject this route without a copy.
  TexResult Init(const std::shared_ptr<CD3D11Device>& d11,
    const std::shared_ptr<CD3D12Device>& d12, UINT bind = 0);
  TexResult Get(const TexLease& src, D11Lease& lease);
  void Reset();
};
