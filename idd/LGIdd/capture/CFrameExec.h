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

#include "Atomic.h"
#include "CSRWLock.h"
#include "capture/CFrameGraph.h"
#include "capture/CFrameTex.h"
#include "transport/ITexStage.h"

#include <memory>

class CInteropResource;
class CTexHub;
struct CD3D11Device;
struct CD3D12Device;

// Transactional executor for texture-backed transport routes. Every product
// is materialized in executor-owned storage before the acquired IddCx surface
// is released. A graph containing only legacy routes remains fully dormant.
class CFrameExec final : public ITexStage
{
private:
  struct Core;

  mutable CSRWLock               m_lock;
  std::shared_ptr<CD3D11Device>  m_d11;
  std::shared_ptr<CD3D12Device>  m_d12;
  CTexHub                      * m_hub = nullptr;
  std::shared_ptr<Core>          m_active;
  std::shared_ptr<Core>          m_pending;
  std::atomic<uint64_t>          m_serial { 0 };

  uint64_t NextSerial();

public:
  CFrameExec() = default;
  ~CFrameExec();
  CFrameExec(const CFrameExec&) = delete;
  CFrameExec& operator=(const CFrameExec&) = delete;

  bool Init(const std::shared_ptr<CD3D11Device>& d11,
    const std::shared_ptr<CD3D12Device>& d12, CTexHub& hub);
  void Reset();

  CfgResult Prep(const CFrameGraph& graph) noexcept override;
  void Commit() noexcept override;
  void Abort() noexcept override;

  TexResult Run(CInteropResource& src, const D12FrameFormat& format,
    uint64_t captureTime, uint64_t postStart, FrameDamage damage,
    const RECT * rects, unsigned count) noexcept;
};
