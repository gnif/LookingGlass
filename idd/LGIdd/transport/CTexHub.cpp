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

#include "transport/CTexHub.h"

#include "capture/CFrameGraph.h"
#include "capture/CFrameTex.h"
#include "d3d/CInteropPool.h"
#include "transport/ITexSink.h"
#include "transport/ITransport.h"

#include <cstddef>
#include <cstring>
#include <new>
#include <utility>

namespace
{
  template<typename T, std::size_t N>
  T& Append(T (&items)[N], unsigned& count)
  {
    if (count == N)
    {
      for (unsigned i = 1; i < count; ++i)
        items[i - 1] = items[i];
      --count;
    }
    return items[count++];
  }
}

struct CTexSet
{
  struct Route
  {
    std::shared_ptr<ITransport> owner;
    ITexSink                  * sink     = nullptr;
    BackendId                   id       = 0;
    uint32_t                    epoch    = 0;
    unsigned                    node     = 0;
    unsigned                    leaf     = 0;
    FrameProfile                profile;
    FrameCfg                    cfg;
    bool                        required = false;
    bool                        primary  = false;
    bool                        active   = false;
    bool                        dropped  = false;
    CfgResult                   fault    = CfgResult::ACCEPTED;
    unsigned                    calls    = 0;
    HANDLE                      idle     = nullptr;

    ~Route()
    {
      if (idle)
        CloseHandle(idle);
    }
  };

  CSRWLock    lock;
  CFrameGraph graph;
  uint64_t    generation = 0;
  unsigned    count      = 0;
  Route       routes[TRANSPORT_MAX_INSTANCES];
};

static bool SameFrame(const FrameDesc& left, const FrameDesc& right,
  bool ignoreFlags = false)
{
  if (left.content != right.content                           ||
      left.damage  != right.damage                            ||
      left.count   != right.count                             ||
      left.count   > FRAME_DAMAGE_MAX                         ||
      !D12::Same(left.format, right.format, ignoreFlags ?
        D12::FormatCmp::NO_FLAGS : D12::FormatCmp::EXACT))
    return false;
  return !left.count || memcmp(left.rects, right.rects,
    left.count * sizeof(*left.rects)) == 0;
}

static unsigned Find(const CTexSet& set, const FrameIn& frame)
{
  for (unsigned i = 0; i < set.count; ++i)
  {
    const CTexSet::Route& route = set.routes[i];
    if (route.id    == frame.desc.id    &&
        route.epoch == frame.desc.epoch &&
        route.node  == frame.desc.node  &&
        Frame::Same(route.profile, frame.desc.profile))
      return i;
  }
  return TRANSPORT_MAX_INSTANCES;
}

static bool Valid(const CTexSet& set, const CTexSet::Route& route,
  const FrameIn& frame, const CFrameTex& tex, FrameStorage storage)
{
  const FrameProfile product =
    Frame::Store(route.profile, FrameStorage::D3D12_TEXTURE);
  LeafDesc expected;
  if (!set.graph.Desc(route.leaf, frame.desc.frame.content, expected) ||
      expected.id    != frame.desc.id                                ||
      expected.epoch != frame.desc.epoch                             ||
      expected.node  != frame.desc.node                              ||
      !Frame::Same(expected.profile, frame.desc.profile)              ||
      !SameFrame(expected.frame, frame.desc.frame)                     ||
      !SameFrame(expected.frame, tex.frame, true))
    return false;
  return
    frame.graph            != 0           &&
    frame.desc.node        != 0           &&
    frame.desc.frame.content              &&
    route.profile.storage  == storage     &&
    tex.graph              == frame.graph &&
    tex.node               == route.node  &&
    Frame::Same(tex.profile, product);
}

CTexStage::CTexStage(CTexStage&& other) noexcept :
  m_owner(other.m_owner), m_next(std::move(other.m_next))
{
  other.m_owner = nullptr;
}

CTexStage& CTexStage::operator=(CTexStage&& other) noexcept
{
  if (this == &other)
    return *this;
  if (m_owner)
    m_owner->Abort(*this);
  m_owner = other.m_owner;
  m_next  = std::move(other.m_next);
  other.m_owner = nullptr;
  return *this;
}

CTexStage::~CTexStage()
{
  if (m_owner)
    m_owner->Abort(*this);
}

CTexHub::CTexHub(std::atomic<uint64_t>& rev) : m_rev(rev)
{
  m_idle = CreateEvent(nullptr, TRUE, TRUE, nullptr);
}

CTexHub::~CTexHub()
{
  Stop();
  if (m_idle)
    CloseHandle(m_idle);
}

bool CTexHub::Enter(const FrameIn& frame, bool lease,
  std::shared_ptr<CTexSet>& set, unsigned& route, ITexSink *& sink)
{
  route = TRANSPORT_MAX_INSTANCES;
  sink  = nullptr;
  {
    CSRWExclusiveLock lock(m_lock);
    if (!m_open || m_stopped || !m_active)
      return false;
    set = m_active;
    if (!m_calls++)
      ResetEvent(m_idle);
  }

  CSRWExclusiveLock lock(set->lock);
  route = Find(*set, frame);
  if (route >= set->count || !set->routes[route].active || !lease ||
      frame.graph != set->generation)
    return false;

  CTexSet::Route& target = set->routes[route];
  if (!target.calls++)
    ResetEvent(target.idle);
  sink = target.sink;
  return true;
}

void CTexHub::Finish(const std::shared_ptr<CTexSet>& set,
  unsigned route, PushResult result)
{
  if (result == PushResult::REJECTED || result == PushResult::FAILED)
    Disable(set, route, result);
  Leave(set, route);
}

void CTexHub::Bump()
{
  Atomic::Next(m_rev, std::memory_order_release);
}

bool CTexHub::Match(const FaultRec& fault, BackendId id, uint32_t epoch,
  const FrameCfg& cfg, bool anyCfg)
{
  return
    fault.id    == id    &&
    fault.epoch == epoch &&
    (anyCfg || fault.result == CfgResult::FAILED ||
      Frame::Same(fault.cfg, cfg));
}

CfgResult CTexHub::Faulted(BackendId id, uint32_t epoch,
  const FrameCfg& cfg) const
{
  CSRWSharedLock lock(m_lock);
  for (unsigned i = 0; i < m_faultCount; ++i)
    if (Match(m_faults[i], id, epoch, cfg))
      return m_faults[i].result;
  return CfgResult::ACCEPTED;
}

void CTexHub::Rebind(BackendId id, uint32_t epoch)
{
  if (!id || !epoch)
    return;
  CSRWExclusiveLock lock(m_lock);
  for (unsigned i = 0; i < m_faultCount;)
  {
    if (m_faults[i].id != id || m_faults[i].epoch != epoch)
    {
      ++i;
      continue;
    }
    m_faults[i] = m_faults[--m_faultCount];
    m_faults[m_faultCount] = FaultRec {};
  }
}

void CTexHub::Leave(
  const std::shared_ptr<CTexSet>& set, unsigned route)
{
  if (set && route < set->count)
  {
    CSRWExclusiveLock setLock(set->lock);
    CTexSet::Route& target = set->routes[route];
    if (target.calls && !--target.calls && target.idle)
      SetEvent(target.idle);
  }
  CSRWExclusiveLock lock(m_lock);
  if (m_calls && !--m_calls)
    SetEvent(m_idle);
}

void CTexHub::Disable(const std::shared_ptr<CTexSet>& set,
  unsigned route, PushResult result)
{
  bool found = false;
  CTexSet::Route failed;
  {
    CSRWExclusiveLock lock(set->lock);
    if (route < set->count && !set->routes[route].dropped &&
        (set->routes[route].active ||
         (result == PushResult::FAILED &&
          set->routes[route].fault == CfgResult::REJECTED)))
    {
      CTexSet::Route& target = set->routes[route];
      target.active = false;
      target.fault  = result == PushResult::FAILED ?
        CfgResult::FAILED : CfgResult::REJECTED;
      failed.id     = target.id;
      failed.epoch  = target.epoch;
      failed.cfg    = target.cfg;
      found         = true;
    }
  }
  if (!found)
    return;

  FaultRec record;
  record.id     = failed.id;
  record.epoch  = failed.epoch;
  record.cfg    = failed.cfg;
  record.result = result == PushResult::FAILED ?
    CfgResult::FAILED : CfgResult::REJECTED;

  bool wake = false;
  {
    CSRWExclusiveLock lock(m_lock);
    for (unsigned i = 0; i < m_faultCount; ++i)
      if (Match(m_faults[i], failed.id, failed.epoch, failed.cfg,
          result == PushResult::FAILED))
      {
        if (result == PushResult::FAILED &&
            m_faults[i].result != CfgResult::FAILED)
        {
          m_faults[i].result = CfgResult::FAILED;
          wake = true;
        }
        if (result != PushResult::FAILED)
        {
          lock.Unlock();
          if (wake)
            Bump();
          return;
        }
        break;
      }
    bool faulted = false;
    for (unsigned i = 0; i < m_faultCount; ++i)
      if (Match(m_faults[i], failed.id, failed.epoch, failed.cfg))
      {
        faulted = true;
        break;
      }
    if (!faulted)
    {
      Append(m_faults, m_faultCount) = record;
      wake = true;
    }
    if (result == PushResult::FAILED)
    {
      bool queued = false;
      for (unsigned i = 0; i < m_failureCount; ++i)
        if (m_failures[i].id == failed.id &&
            m_failures[i].epoch == failed.epoch)
        {
          queued = true;
          break;
        }
      if (!queued)
      {
        Failure& source = Append(m_failures, m_failureCount);
        source.id    = failed.id;
        source.epoch = failed.epoch;
      }
      wake = true;
    }
  }
  if (wake)
    Bump();
}

bool CTexHub::TakeFailure(BackendId& id, uint32_t& epoch)
{
  CSRWExclusiveLock lock(m_lock);
  if (!m_failureCount)
    return false;
  id    = m_failures[0].id;
  epoch = m_failures[0].epoch;
  for (unsigned i = 1; i < m_failureCount; ++i)
    m_failures[i - 1] = m_failures[i];
  m_failures[--m_failureCount] = Failure {};
  return true;
}

CfgResult CTexHub::Hold(CTexStage& stage)
{
  if (stage.m_owner)
    return CfgResult::REJECTED;
  if (!m_idle)
    return CfgResult::FAILED;
  {
    CSRWExclusiveLock lock(m_lock);
    if (m_stopped || m_changing || m_pending)
      return CfgResult::RETRY;
    m_changing    = true;
    m_open        = false;
    stage.m_owner = this;
    if (m_calls)
      ResetEvent(m_idle);
    else
      SetEvent(m_idle);
  }
  return WaitForSingleObject(m_idle, INFINITE) == WAIT_OBJECT_0 ?
    CfgResult::ACCEPTED : CfgResult::FAILED;
}

CfgResult CTexHub::Prep(const CFrameGraph& graph,
  const Bind * binds, unsigned count, CTexStage& stage)
{
  if (stage.m_owner != this || stage.m_next || !graph.Generation() ||
      count > TRANSPORT_MAX_INSTANCES || (count && !binds))
    return CfgResult::REJECTED;

  unsigned leafCount = 0;
  const GraphLeaf * leaves = graph.Leaves(leafCount);
  if (!leaves || leafCount != count)
    return CfgResult::REJECTED;

  std::shared_ptr<CTexSet> next;
  try
  {
    next = std::make_shared<CTexSet>();
  }
  catch (const std::bad_alloc&)
  {
    return CfgResult::FAILED;
  }
  next->generation = graph.Generation();
  next->graph      = graph;

  for (unsigned leaf = 0; leaf < leafCount; ++leaf)
  {
    const GraphLeaf& route = leaves[leaf];
    const Bind * binding = nullptr;
    for (unsigned bind = 0; bind < count; ++bind)
      if (binds[bind].id == route.id && binds[bind].epoch == route.epoch)
      {
        if (binding)
          return CfgResult::FAILED;
        binding = &binds[bind];
      }
    if (!binding || !binding->owner)
      return CfgResult::FAILED;

    // Legacy transports deliberately have no texture sink yet. Their normal
    // frame service remains active while this route table stays dormant.
    if (!binding->sink)
      continue;

    CTexSet::Route& target = next->routes[next->count++];
    target.owner    = binding->owner;
    target.sink     = binding->sink;
    target.id       = route.id;
    target.epoch    = route.epoch;
    target.node     = route.node;
    target.leaf     = leaf;
    target.profile  = route.cfg.profile;
    target.cfg      = route.cfg;
    target.required = route.required;
    target.primary  = route.primary;
    target.active   = true;
    target.idle     = CreateEvent(nullptr, TRUE, TRUE, nullptr);
    if (!target.idle)
      return CfgResult::FAILED;
  }

  {
    CSRWExclusiveLock lock(m_lock);
    if (m_stopped || !m_changing || m_open || m_pending)
      return CfgResult::RETRY;
    m_pending    = next;
    stage.m_next = next;
  }
  return CfgResult::ACCEPTED;
}

void CTexHub::Commit(CTexStage& stage) noexcept
{
  std::shared_ptr<CTexSet> old;
  {
    CSRWExclusiveLock lock(m_lock);
    if (stage.m_owner != this || stage.m_next != m_pending)
      return;
    old           = std::move(m_active);
    m_active      = std::move(m_pending);
    m_open        = !m_stopped;
    m_changing    = false;
    stage.m_owner = nullptr;
    stage.m_next.reset();
  }
  old.reset();
}

void CTexHub::Abort(CTexStage& stage) noexcept
{
  std::shared_ptr<CTexSet> drop;
  {
    CSRWExclusiveLock lock(m_lock);
    if (stage.m_owner != this)
      return;
    if (stage.m_next == m_pending)
    {
      drop          = std::move(m_pending);
      m_open        = !m_stopped;
      m_changing    = false;
    }
    stage.m_owner = nullptr;
    stage.m_next.reset();
  }
  drop.reset();
}

PushResult CTexHub::Push(FrameIn frame, TexLease lease) noexcept
{
  std::shared_ptr<CTexSet> set;
  unsigned index;
  ITexSink * sink = nullptr;
  if (!Enter(frame, !!lease, set, index, sink))
  {
    if (set)
      Leave(set, TRANSPORT_MAX_INSTANCES);
    return PushResult::STALE;
  }

  const D12SyncState state = lease->Status();
  bool valid;
  {
    CSRWSharedLock lock(set->lock);
    valid = sink                                                    &&
      Valid(*set, set->routes[index], frame, *lease.Get(),
        FrameStorage::D3D12_TEXTURE)                                &&
      state != D12SyncState::FAILED                                 &&
      (!lease->time.readyValid || state == D12SyncState::READY);
  }
  if (!valid)
  {
    const PushResult result = state == D12SyncState::FAILED && sink ?
      PushResult::FAILED : PushResult::STALE;
    Finish(set, index, result);
    return result;
  }

  const PushResult result = sink->Push(std::move(frame), std::move(lease));
  Finish(set, index, result);
  return result;
}

PushResult CTexHub::Push(FrameIn frame, D11Lease lease) noexcept
{
  std::shared_ptr<CTexSet> set;
  unsigned index;
  ITexSink * sink = nullptr;
  if (!Enter(frame, !!lease, set, index, sink))
  {
    if (set)
      Leave(set, TRANSPORT_MAX_INSTANCES);
    return PushResult::STALE;
  }

  const D12SyncState state       = lease.Status();
  const CFrameTex  * source      = lease.Frame();
  const D12SyncState sourceState = source ?
    source->Status() : D12SyncState::FAILED;
  bool valid;
  {
    CSRWSharedLock lock(set->lock);
    const CTexSet::Route& route = set->routes[index];
    valid = sink                                                   &&
      source                                                       &&
      Frame::Same(lease.Profile(), route.profile)                  &&
      Valid(*set, route, frame, *source,
        FrameStorage::D3D11_TEXTURE)                               &&
      state       != D12SyncState::FAILED                          &&
      sourceState != D12SyncState::FAILED                          &&
      (!source->time.readyValid ||
       (state == D12SyncState::READY &&
        sourceState == D12SyncState::READY));
  }
  if (!valid)
  {
    const PushResult result = sink &&
      (state == D12SyncState::FAILED ||
       sourceState == D12SyncState::FAILED) ?
      PushResult::FAILED : PushResult::STALE;
    Finish(set, index, result);
    return result;
  }

  const PushResult result = sink->Push(std::move(frame), std::move(lease));
  Finish(set, index, result);
  return result;
}

void CTexHub::Drop(BackendId id, uint32_t epoch)
{
  if (!id || !epoch)
    return;

  std::shared_ptr<CTexSet> set;
  {
    CSRWSharedLock lock(m_lock);
    if (m_stopped)
      return;
    set = m_active;
  }
  if (!set)
  {
    Rebind(id, epoch);
    return;
  }

  HANDLE waits[TRANSPORT_MAX_INSTANCES] = {};
  unsigned waitCount = 0;
  bool changed = false;
  {
    CSRWExclusiveLock lock(set->lock);
    for (unsigned i = 0; i < set->count; ++i)
      if (set->routes[i].id == id && set->routes[i].epoch == epoch)
      {
        CTexSet::Route& target = set->routes[i];
        changed       |= target.active;
        target.active  = false;
        target.dropped = true;
        if (target.calls)
        {
          ResetEvent(target.idle);
          waits[waitCount++] = target.idle;
        }
      }
  }
  for (unsigned i = 0; i < waitCount; ++i)
    WaitForSingleObject(waits[i], INFINITE);
  Rebind(id, epoch);
  if (changed)
    Bump();
}

void CTexHub::Stop()
{
  std::shared_ptr<CTexSet> active;
  std::shared_ptr<CTexSet> pending;
  {
    CSRWExclusiveLock lock(m_lock);
    if (m_stopped)
      return;
    m_stopped  = true;
    m_changing = true;
    m_open     = false;
    if (m_calls)
      ResetEvent(m_idle);
    else if (m_idle)
      SetEvent(m_idle);
  }

  if (m_idle)
    WaitForSingleObject(m_idle, INFINITE);
  {
    CSRWExclusiveLock lock(m_lock);
    active  = std::move(m_active);
    pending = std::move(m_pending);
  }
  pending.reset();
  active.reset();
}
