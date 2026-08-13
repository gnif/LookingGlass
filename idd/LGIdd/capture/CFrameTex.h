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
#include "capture/CFrameGraph.h"
#include "d3d/CD3D12CommandQueue.h"

#include <memory>

struct CTexCore;
class TexWrite;

enum class TexResult : uint8_t
{
  OK,
  BUSY,
  REJECTED,
  FAILED,
};

// Immutable producer boundaries for one node product. A transport adds its
// own transfer or encode timing without rewriting these common values.
struct FrameTime
{
  uint64_t postStart  = 0;
  uint64_t gpuStart   = 0;
  uint64_t gpuEnd     = 0;
  uint64_t readyAt    = 0;
  bool     gpuValid   = false;
  bool     readyValid = false;
};

// An immutable, pool-owned D3D12 graph product. Node zero is deliberately
// excluded because it aliases the acquired IddCx surface.
class CFrameTex final
{
  friend class TexWrite;

private:
  ComPtr<ID3D12Resource>  m_res;
  D12Sync                 m_sync;
  D3D12_RESOURCE_STATES   m_state;
  bool                     m_shared;

  CFrameTex(uint64_t graph, uint64_t pool, unsigned node, unsigned slot,
    uint64_t version, const FrameProfile& profile, const FrameDesc& frame,
    const FrameTime& time, const ComPtr<ID3D12Resource>& res,
    const D12Sync& sync,
    D3D12_RESOURCE_STATES state, bool shared);

public:
  CFrameTex(const CFrameTex&) = delete;
  CFrameTex& operator=(const CFrameTex&) = delete;

  const uint64_t     graph;
  const uint64_t     pool;
  const unsigned     node;
  const unsigned     slot;
  const uint64_t     version;
  const FrameProfile profile;
  const FrameDesc    frame;
  const FrameTime    time;

  ID3D12Resource * Get() const { return m_res.Get(); }
  const D12Sync& Sync() const { return m_sync; }
  D3D12_RESOURCE_STATES State() const { return m_state; }
  bool Shared() const { return m_shared; }
  bool Wait(CD3D12CommandSlot& cmd) const;
  D12SyncState Status() const { return m_sync.State(); }
  bool Ready() const { return m_sync.Done(); }
};

// Consumers retain a lease until their own GPU use of the texture has
// completed. Dropping the CPU call's local copy is not sufficient.
class TexLease
{
  friend class TexWrite;

private:
  std::shared_ptr<const CFrameTex> m_tex;
  explicit TexLease(std::shared_ptr<const CFrameTex> tex) : m_tex(tex) {}

public:
  TexLease() = default;
  TexLease(const TexLease&) = default;
  TexLease& operator=(const TexLease&) = default;
  TexLease(TexLease&&) noexcept = default;
  TexLease& operator=(TexLease&&) noexcept = default;

  explicit operator bool() const { return !!m_tex; }
  const CFrameTex * Get() const { return m_tex.get(); }
  const CFrameTex * operator->() const { return m_tex.get(); }
  void Reset() { m_tex.reset(); }
};

// A move-only producer reservation. Abandoning a writer retires its slot;
// Cancel may be used only before any command referencing the texture submits.
class TexWrite
{
  friend class CTexPool;

private:
  std::shared_ptr<CTexCore> m_core;
  unsigned                  m_index   = 0;
  uint64_t                  m_version = 0;

  TexWrite(const std::shared_ptr<CTexCore>& core,
    unsigned index, uint64_t version);
  void Clear();
  void Retire();

public:
  TexWrite() = default;
  TexWrite(const TexWrite&) = delete;
  TexWrite& operator=(const TexWrite&) = delete;
  TexWrite(TexWrite&& other) noexcept;
  TexWrite& operator=(TexWrite&& other) noexcept;
  ~TexWrite();

  explicit operator bool() const { return !!m_core; }
  ID3D12Resource * Get() const;
  // Seal only after producer commands restore the texture to the pool's
  // configured immutable state and Execute returns this exact sync point.
  bool Seal(const FrameDesc& frame, const FrameTime& time,
    const D12Sync& sync, TexLease& lease);
  void Cancel();
};

// One pool owns interchangeable textures for a single graph node. Reset stops
// acquisition and drains every sealed producer point. Existing writers and
// leases keep the retired core alive. Reset invalidates writers; a writer
// which already submitted must still Seal so its point is safely drained,
// but it will not publish a product from the retired generation.
// The configured state is the promised state presented to every consumer;
// it is not queried from D3D12. Consumers may read but never transition or
// write a shared product.
class CTexPool
{
private:
  mutable CSRWLock          m_lock;
  std::shared_ptr<CTexCore> m_core;

public:
  static const unsigned MAX_SLOTS = 4;

  CTexPool() = default;
  CTexPool(const CTexPool&) = delete;
  CTexPool& operator=(const CTexPool&) = delete;
  ~CTexPool() { Reset(); }

  TexResult Init(ID3D12Device3 * device, uint64_t graph, unsigned node,
    const FrameProfile& profile, const D3D12_RESOURCE_DESC& desc,
    D3D12_RESOURCE_STATES state, unsigned slots, bool shared = false);
  void Reset();
  TexResult Try(TexWrite& write);
};
