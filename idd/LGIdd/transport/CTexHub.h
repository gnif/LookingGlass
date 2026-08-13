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
#include "transport/FrameIn.h"
#include "transport/FrameProfile.h"
#include "transport/TransportConfig.h"

#include <Windows.h>
#include <memory>

class CFrameGraph;
class CTransportManager;
class D11Lease;
class ITransport;
class ITexSink;
class TexLease;
class CTexHub;
struct CTexSet;

class CTexStage
{
  friend class CTexHub;

private:
  CTexHub                * m_owner = nullptr;
  std::shared_ptr<CTexSet> m_next;

public:
  CTexStage() = default;
  CTexStage(const CTexStage&) = delete;
  CTexStage& operator=(const CTexStage&) = delete;
  CTexStage(CTexStage&& other) noexcept;
  CTexStage& operator=(CTexStage&& other) noexcept;
  ~CTexStage();
};

class CTexHub
{
  friend class CTransportManager;
  friend class CTexStage;

private:
  struct Bind
  {
    std::shared_ptr<ITransport> owner;
    BackendId                   id    = 0;
    uint32_t                    epoch = 0;
    ITexSink                  * sink  = nullptr;
  };

  struct FaultRec
  {
    BackendId    id    = 0;
    uint32_t     epoch = 0;
    FrameCfg     cfg;
    CfgResult    result = CfgResult::ACCEPTED;
  };

  struct Failure
  {
    BackendId id    = 0;
    uint32_t  epoch = 0;
  };

  static const unsigned MAX_FAULTS =
    TRANSPORT_MAX_INSTANCES * FRAME_PROFILE_MAX;

  mutable CSRWLock              m_lock;
  std::shared_ptr<CTexSet>      m_active;
  std::shared_ptr<CTexSet>      m_pending;
  std::atomic<uint64_t>       & m_rev;
  HANDLE                        m_idle                              = nullptr;
  unsigned                      m_calls                             = 0;
  bool                          m_open                              = true;
  bool                          m_changing                          = false;
  bool                          m_stopped                           = false;
  FaultRec                      m_faults[MAX_FAULTS]                = {};
  unsigned                      m_faultCount                        = 0;
  Failure                       m_failures[TRANSPORT_MAX_INSTANCES] = {};
  unsigned                      m_failureCount                      = 0;

  static bool Match(const FaultRec& fault, BackendId id, uint32_t epoch,
    const FrameCfg& cfg, bool anyCfg = false);

  bool Enter(const FrameIn& frame, bool lease,
    std::shared_ptr<CTexSet>& set, unsigned& route, ITexSink *& sink);
  void Finish(const std::shared_ptr<CTexSet>& set, unsigned route,
    PushResult result);
  void Leave(const std::shared_ptr<CTexSet>& set, unsigned route);
  void Disable(const std::shared_ptr<CTexSet>& set, unsigned route,
    PushResult result);
  void Bump();

  explicit CTexHub(std::atomic<uint64_t>& rev);
  ~CTexHub();

  CTexHub(const CTexHub&) = delete;
  CTexHub& operator=(const CTexHub&) = delete;

  CfgResult Hold(CTexStage& stage);
  CfgResult Prep(const CFrameGraph& graph,
    const Bind * binds, unsigned count, CTexStage& stage);
  void Commit(CTexStage& stage) noexcept;
  void Abort(CTexStage& stage) noexcept;

  CfgResult Faulted(BackendId id, uint32_t epoch,
    const FrameCfg& cfg) const;
  bool TakeFailure(BackendId& id, uint32_t& epoch);
  void Rebind(BackendId id, uint32_t epoch);

  // Drop closes only this route and drains its calls before the transport can
  // stop. Accepted asynchronous leases remain the transport's responsibility.
  void Drop(BackendId id, uint32_t epoch);
  void Stop();

public:
  PushResult Push(FrameIn frame, TexLease lease) noexcept;
  PushResult Push(FrameIn frame, D11Lease lease) noexcept;
};
