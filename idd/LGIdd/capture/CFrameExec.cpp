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

#include "capture/CFrameExec.h"

#include "d3d/CD3D11Device.h"
#include "d3d/CD3D12Device.h"
#include "d3d/CInteropPool.h"
#include "d3d/CInteropResource.h"
#include "postprocess/effect/CColorTransformEffect.h"
#include "postprocess/effect/CDownsampleEffect.h"
#include "postprocess/effect/CFormatEffect.h"
#include "postprocess/effect/CHDR16to10Effect.h"
#include "transport/CTexHub.h"

#include <cstring>
#include <new>
#include <utility>

namespace
{
  static const unsigned EXEC_LANES = 2;
  static const unsigned EXEC_SLOTS = 4;

  enum class ExecOp : uint8_t
  {
    COPY,
    CAL,
    LUT,
    SCALE,
    HDR10,
    FORMAT,
  };

  D12FrameFormat MakeFormat(const FrameProfile& profile,
    unsigned width, unsigned height,
    const std::shared_ptr<const D12ColorTransform>& transform,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
  {
    D12FrameFormat format = {};
    D12::Set(format,
      Frame::Store(profile, FrameStorage::D3D12_TEXTURE),
      width, height, flags);
    format.colorTransform = transform;
    return format;
  }

  bool Copy(ID3D12GraphicsCommandList * list,
    ID3D12Resource * src, ID3D12Resource * dst)
  {
    if (!list || !src || !dst || src == dst)
      return false;

    const D3D12_RESOURCE_DESC in  = src->GetDesc();
    const D3D12_RESOURCE_DESC out = dst->GetDesc();
    if (!D12::Same(in, out, D12::DescCmp::COPY))
      return false;

    // Leave the immutable input in COMMON. COPY_SOURCE is promoted
    // implicitly, allowing the legacy and texture queues to read it without
    // cross-queue state ownership. Only this private destination transitions.
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = dst;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
    list->CopyResource(dst, src);
    std::swap(barrier.Transition.StateBefore,
      barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
    return true;
  }

  CfgResult CfgFromTex(TexResult result)
  {
    switch (result)
    {
      case TexResult::OK:
        return CfgResult::ACCEPTED;
      case TexResult::BUSY:
        return CfgResult::RETRY;
      case TexResult::REJECTED:
        return CfgResult::REJECTED;
      case TexResult::FAILED:
        return CfgResult::FAILED;
    }
    return CfgResult::FAILED;
  }

  bool Supports(ID3D12Device3 * device, ExecOp op,
    DXGI_FORMAT src, DXGI_FORMAT dst)
  {
    if (op == ExecOp::COPY)
      return true;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT input = { src };
    D3D12_FEATURE_DATA_FORMAT_SUPPORT output = { dst };
    if (!device || FAILED(device->CheckFeatureSupport(
          D3D12_FEATURE_FORMAT_SUPPORT, &input, sizeof(input))) ||
        FAILED(device->CheckFeatureSupport(
          D3D12_FEATURE_FORMAT_SUPPORT, &output, sizeof(output))))
      return false;
    const D3D12_FORMAT_SUPPORT1 read = op == ExecOp::SCALE ?
      D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE :
      D3D12_FORMAT_SUPPORT1_SHADER_LOAD;
    return (input.Support1 & read) == read &&
      (output.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
  }
}

struct CFrameExec::Core : std::enable_shared_from_this<CFrameExec::Core>
{
  struct Fx
  {
    ExecOp                                  op = ExecOp::COPY;
    ComPtr<ID3D12Resource>                  scratch;
    std::unique_ptr<CColorTransformEffect>  color;
    std::unique_ptr<CDownsampleEffect>      scale;
    std::unique_ptr<CHDR16to10Effect>       hdr;
    std::unique_ptr<CFormatEffect>          convert;

    bool Init(const ComPtr<ID3D12Device3>& device, ExecOp selected,
      const D12FrameFormat& src, const D12FrameFormat& dst, bool exported)
    {
      op = selected;

      PostProcessStatus status = PostProcessStatus::SUCCESS;
      D12FrameFormat    output = src;
      output.desc.Flags        = dst.desc.Flags;
      switch (op)
      {
        case ExecOp::COPY:
          if (src.desc.Width  != dst.desc.Width  ||
              src.desc.Height != dst.desc.Height ||
              src.desc.Format != dst.desc.Format)
            return false;
          break;

        case ExecOp::CAL:
          color.reset(new (std::nothrow)
            CColorTransformEffect(CalPart::MATRIX, true));
          if (!color || !color->Init(device))
            return false;
          status = color->Cfg(device, src, output);
          break;

        case ExecOp::LUT:
          color.reset(new (std::nothrow)
            CColorTransformEffect(CalPart::LUT, true));
          if (!color || !color->Init(device))
            return false;
          status = color->Cfg(device, src, output);
          break;

        case ExecOp::SCALE:
          scale.reset(new (std::nothrow)
            CDownsampleEffect(dst.width, dst.height));
          if (!scale || !scale->Init(device))
            return false;
          status = scale->Cfg(device, src, output);
          break;

        case ExecOp::HDR10:
          hdr.reset(new (std::nothrow) CHDR16to10Effect);
          if (!hdr || !hdr->Init(device))
            return false;
          status = hdr->Cfg(device, src, output);
          break;

        case ExecOp::FORMAT:
          convert.reset(new (std::nothrow) CFormatEffect);
          if (!convert || !convert->Init(device))
            return false;
          status = convert->Cfg(device, src, dst);
          output = dst;
          break;
      }

      if (status != PostProcessStatus::SUCCESS ||
          !D12::Same(output, dst, D12::FormatCmp::IMAGE))
        return false;

      if (!exported && !PostProcessUtil::CreateDefaultTexture(
          device, dst.desc, scratch))
        return false;
      return true;
    }

    bool Run(const ComPtr<ID3D12Device3>& device,
      const ComPtr<ID3D12GraphicsCommandList>& list,
      ID3D12Resource * src, ID3D12Resource * dst)
    {
      if (!src || !dst)
        return false;
      if (op == ExecOp::COPY)
        return Copy(list.Get(), src, dst);

      ComPtr<ID3D12Resource> input = src;
      RECT rect = {};
      unsigned count = 0;
      switch (op)
      {
        case ExecOp::CAL:
        case ExecOp::LUT:
          return color && color->Run(
            device, list, input, dst, &rect, &count);
        case ExecOp::SCALE:
          return scale && scale->Run(
            device, list, input, dst, &rect, &count);
        case ExecOp::HDR10:
          return hdr && hdr->Run(
            device, list, input, dst, &rect, &count);
        case ExecOp::FORMAT:
          return convert && convert->Run(
            device, list, input, dst, &rect, &count);
        case ExecOp::COPY:
          break;
      }
      return false;
    }
  };

  struct Lane
  {
    Fx nodes[FRAME_GRAPH_MAX_NODES];
  };

  CSRWLock                      runLock;
  std::shared_ptr<CD3D11Device> d11;
  std::shared_ptr<CD3D12Device> d12;
  ComPtr<ID3D12Device3>         device;
  CTexHub                      * hub = nullptr;
  CFrameGraph                   graph;
  CD3D12CommandQueue            queue;
  CTexPool                      pools[FRAME_GRAPH_MAX_NODES];
  CInteropPool                  interop[FRAME_GRAPH_MAX_NODES];
  Lane                          lanes[EXEC_LANES];
  D12FrameFormat                formats[FRAME_GRAPH_MAX_NODES]  = {};
  ExecOp                        ops[FRAME_GRAPH_MAX_NODES]      = {};
  bool                          exported[FRAME_GRAPH_MAX_NODES] = {};
  bool                          d11Node[FRAME_GRAPH_MAX_NODES]  = {};
  unsigned                      readers[FRAME_GRAPH_MAX_NODES]  = {};
  unsigned                      nodeCount                       = 0;
  unsigned                      leafCount                       = 0;
  unsigned                      nextLane                        = 0;
  bool                          live                            = false;
  std::atomic<bool>             forceFull { true };

  ~Core()
  {
    // Effect descriptor heaps, scratch textures, and pool destinations are
    // referenced by submitted lists. Drain before member destruction.
    queue.WaitForIdle();
  }

  static bool Select(const GraphNode& parent,
    const GraphNode& node, ExecOp& op)
  {
    switch (node.op)
    {
      case FrameOp::CAL:
        op = ExecOp::CAL;
        return true;
      case FrameOp::LUT:
        op = ExecOp::LUT;
        return true;
      case FrameOp::SCALE:
        op = ExecOp::SCALE;
        return true;
      case FrameOp::HDR10:
        if (parent.profile.signal == FrameSignal::SCRGB_LINEAR &&
            node.profile.signal == FrameSignal::PQ_BT2020)
        {
          op = ExecOp::HDR10;
          return true;
        }
        break;
      case FrameOp::SDR:
      case FrameOp::SCRGB:
        break;
      case FrameOp::SRC:
        return false;
    }

    if (parent.width == node.width && parent.height == node.height &&
        Frame::Same(parent.profile, node.profile))
    {
      op = ExecOp::COPY;
      return true;
    }
    if (parent.width == node.width && parent.height == node.height &&
        parent.profile.signal == FrameSignal::SRGB &&
        node.profile.signal == FrameSignal::SRGB &&
        parent.profile.pixel != node.profile.pixel)
    {
      op = ExecOp::FORMAT;
      return true;
    }
    return false;
  }

  CfgResult Init(const CFrameGraph& source,
    const std::shared_ptr<CD3D11Device>& d11Device,
    const std::shared_ptr<CD3D12Device>& d12Device, CTexHub& texHub)
  {
    d11    = d11Device;
    d12    = d12Device;
    hub    = &texHub;
    graph  = source;
    device = d12 ? d12->GetDevice() : nullptr;
    if (!d11 || !d12 || !device || !graph.Generation())
      return CfgResult::FAILED;

    const GraphNode * nodes = graph.Nodes(nodeCount);
    const GraphLeaf * leaves = graph.Leaves(leafCount);
    if (!nodes || !leaves || !nodeCount ||
        nodeCount > FRAME_GRAPH_MAX_NODES ||
        leafCount > TRANSPORT_MAX_INSTANCES)
      return CfgResult::REJECTED;

    unsigned texLeaves = 0;
    for (unsigned leaf = 0; leaf < leafCount; ++leaf)
      if (leaves[leaf].tex)
      {
        if (!leaves[leaf].node || leaves[leaf].node >= nodeCount)
          return CfgResult::REJECTED;
        exported[leaves[leaf].node] = true;
        ++readers[leaves[leaf].node];
        d11Node[leaves[leaf].node] |=
          leaves[leaf].cfg.profile.storage ==
            FrameStorage::D3D11_TEXTURE;
        ++texLeaves;
      }

    if (!texLeaves)
      return CfgResult::ACCEPTED;
    if (!nodes[0].texRefs || !queue.Init(device.Get(),
        D3D12_COMMAND_LIST_TYPE_DIRECT, L"Frame Graph",
        CD3D12CommandSlot::FAST, EXEC_LANES, false, true))
      return CfgResult::FAILED;

    const GraphCfg& cfg = graph.Cfg();
    formats[0] = MakeFormat(cfg.src, cfg.srcWidth, cfg.srcHeight,
      cfg.transform);

    for (unsigned node = 1; node < nodeCount; ++node)
    {
      if (!nodes[node].texRefs)
        continue;
      if (nodes[node].texRefs > nodes[node].refs ||
          nodes[node].parent >= node ||
          !nodes[nodes[node].parent].texRefs ||
          !Select(nodes[nodes[node].parent], nodes[node], ops[node]))
        return CfgResult::REJECTED;

      D3D12_RESOURCE_FLAGS flags = ops[node] == ExecOp::COPY &&
        exported[node] ?
        D3D12_RESOURCE_FLAG_NONE :
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      if (exported[node] && readers[node] > 1)
        flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
      formats[node] = MakeFormat(nodes[node].profile,
        nodes[node].width, nodes[node].height, cfg.transform, flags);
      if (!Supports(device.Get(), ops[node],
          formats[nodes[node].parent].desc.Format,
          formats[node].desc.Format))
        return CfgResult::REJECTED;

      if (exported[node])
      {
        const FrameProfile profile = Frame::Store(
          nodes[node].profile, FrameStorage::D3D12_TEXTURE);
        const TexResult result = pools[node].Init(device.Get(),
          graph.Generation(), node, profile, formats[node].desc,
          D3D12_RESOURCE_STATE_COMMON, EXEC_SLOTS, graph.Shared(node));
        if (result != TexResult::OK)
          return CfgFromTex(result);
        if (d11Node[node])
        {
          const TexResult interopResult = interop[node].Init(d11, d12);
          if (interopResult != TexResult::OK)
            return CfgFromTex(interopResult);
        }
      }
    }

    for (unsigned lane = 0; lane < EXEC_LANES; ++lane)
      for (unsigned node = 1; node < nodeCount; ++node)
        if (nodes[node].texRefs && !lanes[lane].nodes[node].Init(
            device, ops[node], formats[nodes[node].parent], formats[node],
            exported[node]))
          return CfgResult::REJECTED;

    live = true;
    return CfgResult::ACCEPTED;
  }

  FrameContentRef Content(uint64_t serial, const D12FrameFormat& format,
    uint64_t captureTime, bool full, FrameDamage damage,
    const RECT * rects, unsigned count)
  {
    if (!serial || count > FRAME_DAMAGE_MAX ||
        (count && !rects) ||
        (damage == FrameDamage::RECTS) != (count != 0))
      return {};

    FrameContent * raw = new (std::nothrow) FrameContent;
    if (!raw)
      return {};
    raw->serial      = serial;
    raw->captureTime = captureTime;
    raw->damage      = full ? FrameDamage::FULL : damage;
    raw->count       = raw->damage == FrameDamage::RECTS ? count : 0;
    if (raw->count)
      memcpy(raw->rects, rects, raw->count * sizeof(*rects));
    raw->format      = format;

    try
    {
      return FrameContentRef(raw);
    }
    catch (const std::bad_alloc&)
    {
      return {};
    }
  }

  TexResult Run(CInteropResource& src, const D12FrameFormat& sourceFormat,
    uint64_t serial, uint64_t captureTime, uint64_t postStart,
    FrameDamage damage, const RECT * rects, unsigned count) noexcept
  {
    if (!live)
      return TexResult::OK;

    CSRWExclusiveLock run = CSRWExclusiveLock::Try(runLock);
    if (!run)
    {
      Atomic::Store(forceFull, true, std::memory_order_release);
      return TexResult::BUSY;
    }
    const bool full = Atomic::Swap(forceFull, false, std::memory_order_acq_rel);

    ID3D12Resource * source = src.GetRes().Get();
    if (!source)
    {
      Atomic::Store(forceFull, true, std::memory_order_release);
      return TexResult::REJECTED;
    }
    const D3D12_RESOURCE_DESC actual = source->GetDesc();
    if (!D12::Same(actual, sourceFormat.desc, D12::DescCmp::COPY))
    {
      Atomic::Store(forceFull, true, std::memory_order_release);
      return TexResult::REJECTED;
    }

    const FrameContentRef content = Content(
      serial, sourceFormat, captureTime, full, damage, rects, count);
    FrameDesc check;
    unsigned first = 0;
    while (++first < nodeCount && !exported[first]) {}
    if (!content || first == nodeCount ||
        !graph.Desc(first, content, check))
    {
      Atomic::Store(forceFull, true, std::memory_order_release);
      return TexResult::REJECTED;
    }

    CD3D12CommandSlot * slot = nullptr;
    unsigned lane = 0;
    for (unsigned attempt = 0; attempt < EXEC_LANES; ++attempt)
    {
      lane = (nextLane + attempt) % EXEC_LANES;
      slot = queue.Try(lane);
      if (slot)
      {
        nextLane = (lane + 1) % EXEC_LANES;
        break;
      }
    }
    if (!slot)
    {
      Atomic::Store(forceFull, true, std::memory_order_release);
      return queue.Failed() ? TexResult::FAILED : TexResult::BUSY;
    }

    TexWrite writes[FRAME_GRAPH_MAX_NODES];
    const GraphNode * nodes = graph.Nodes(nodeCount);
    TexResult result = TexResult::OK;
    for (unsigned node = 1; node < nodeCount; ++node)
      if (exported[node])
      {
        result = pools[node].Try(writes[node]);
        if (result != TexResult::OK)
          break;
      }
    if (result != TexResult::OK)
    {
      for (unsigned node = 1; node < nodeCount; ++node)
        writes[node].Cancel();
      slot->Cancel();
      Atomic::Store(forceFull, true, std::memory_order_release);
      return result;
    }

    if (!src.Signal() || !src.Sync(*slot))
    {
      for (unsigned node = 1; node < nodeCount; ++node)
        writes[node].Cancel();
      slot->Cancel();
      Atomic::Store(forceFull, true, std::memory_order_release);
      return TexResult::FAILED;
    }

    ComPtr<ID3D12GraphicsCommandList> list = slot->GetGfxList();
    ID3D12Resource * outputs[FRAME_GRAPH_MAX_NODES] = {};
    outputs[0] = source;
    bool recorded = !!list && !!outputs[0];
    for (unsigned node = 1; recorded && node < nodeCount; ++node)
      if (nodes[node].texRefs)
      {
        ID3D12Resource * output = exported[node] ?
          writes[node].Get() : lanes[lane].nodes[node].scratch.Get();
        recorded = output && lanes[lane].nodes[node].Run(
          device, list, outputs[nodes[node].parent], output);
        outputs[node] = output;
      }

    if (!recorded)
    {
      for (unsigned node = 1; node < nodeCount; ++node)
        writes[node].Cancel();
      slot->Cancel();
      Atomic::Store(forceFull, true, std::memory_order_release);
      return TexResult::FAILED;
    }

    D12Sync sync;
    if (!slot->Execute(&sync))
    {
      const bool submitted = slot->HasSubmittedWork();
      for (unsigned node = 1; node < nodeCount; ++node)
        if (submitted)
          writes[node].Fail(sync);
        else
          writes[node].Cancel();
      Atomic::Store(forceFull, true, std::memory_order_release);
      return TexResult::FAILED;
    }

    const FrameTime time = { postStart, 0, 0, 0, false, false };
    TexLease leases[FRAME_GRAPH_MAX_NODES];
    const std::shared_ptr<void> hold = shared_from_this();
    bool sealed = true;
    for (unsigned node = 1; node < nodeCount; ++node)
      if (exported[node])
      {
        FrameDesc desc;
        if (!graph.Desc(node, content, desc) ||
            !writes[node].Seal(desc, time, sync, leases[node], hold))
        {
          sealed = false;
          break;
        }
      }
    if (!sealed)
    {
      for (unsigned node = 1; node < nodeCount; ++node)
      {
        if (writes[node])
          writes[node].Fail(sync);
        leases[node].Reset();
      }
      Atomic::Store(forceFull, true, std::memory_order_release);
      return TexResult::FAILED;
    }

    bool dropped = false;
    bool failed  = false;
    const GraphLeaf * leaves = graph.Leaves(leafCount);
    for (unsigned leaf = 0; leaf < leafCount; ++leaf)
    {
      if (!leaves[leaf].tex)
        continue;

      FrameIn frame;
      frame.graph = graph.Generation();
      if (!graph.Desc(leaf, content, frame.desc))
      {
        dropped = true;
        continue;
      }

      PushResult pushed = PushResult::STALE;
      const unsigned node = leaves[leaf].node;
      if (leaves[leaf].cfg.profile.storage ==
          FrameStorage::D3D12_TEXTURE)
      {
        pushed = hub->Push(frame, leases[node]);
      }
      else
      {
        D11Lease lease;
        const TexResult opened = interop[node].Get(leases[node], lease);
        if (opened == TexResult::OK)
          pushed = hub->Push(frame, std::move(lease));
        else if (opened == TexResult::BUSY)
          pushed = PushResult::BUSY;
        else
        {
          pushed = opened == TexResult::REJECTED ?
            PushResult::REJECTED : PushResult::FAILED;
          hub->Fault(frame, pushed);
        }
      }

      if (pushed == PushResult::BUSY || pushed == PushResult::STALE)
        dropped = true;
      else if (pushed == PushResult::FAILED)
        failed = true;
    }

    if (dropped)
      Atomic::Store(forceFull, true, std::memory_order_release);
    if (failed)
      return TexResult::FAILED;
    return dropped ? TexResult::BUSY : TexResult::OK;
  }
};

CFrameExec::~CFrameExec()
{
  Reset();
}

bool CFrameExec::Init(const std::shared_ptr<CD3D11Device>& d11,
  const std::shared_ptr<CD3D12Device>& d12, CTexHub& hub)
{
  if (!d11 || !d12 || !d11->GetDevice() || !d12->GetDevice())
    return false;
  CSRWExclusiveLock lock(m_lock);
  if (m_d11 || m_d12 || m_hub)
    return false;
  m_d11 = d11;
  m_d12 = d12;
  m_hub = &hub;
  return true;
}

void CFrameExec::Reset()
{
  std::shared_ptr<Core> active;
  std::shared_ptr<Core> pending;
  {
    CSRWExclusiveLock lock(m_lock);
    active  = std::move(m_active);
    pending = std::move(m_pending);
    m_hub   = nullptr;
    m_d12.reset();
    m_d11.reset();
  }
  pending.reset();
  active.reset();
}

uint64_t CFrameExec::NextSerial()
{
  return Atomic::Next(m_serial);
}

CfgResult CFrameExec::Prep(const CFrameGraph& graph) noexcept
{
  std::shared_ptr<CD3D11Device> d11;
  std::shared_ptr<CD3D12Device> d12;
  CTexHub * hub = nullptr;
  {
    CSRWSharedLock lock(m_lock);
    if (m_pending)
      return CfgResult::REJECTED;
    d11 = m_d11;
    d12 = m_d12;
    hub = m_hub;
  }
  if (!d11 || !d12 || !hub)
    return CfgResult::FAILED;

  std::shared_ptr<Core> next;
  try
  {
    next.reset(new (std::nothrow) Core);
  }
  catch (const std::bad_alloc&)
  {
    return CfgResult::FAILED;
  }
  if (!next)
    return CfgResult::FAILED;

  CfgResult result = CfgResult::FAILED;
  try
  {
    result = next->Init(graph, d11, d12, *hub);
  }
  catch (...)
  {
    return CfgResult::FAILED;
  }
  if (result != CfgResult::ACCEPTED)
    return result;

  {
    CSRWExclusiveLock lock(m_lock);
    if (m_pending || m_d11 != d11 || m_d12 != d12 || m_hub != hub)
      return CfgResult::RETRY;
    m_pending = next;
  }
  return CfgResult::ACCEPTED;
}

void CFrameExec::Commit() noexcept
{
  std::shared_ptr<Core> old;
  {
    CSRWExclusiveLock lock(m_lock);
    if (!m_pending)
      return;
    old      = std::move(m_active);
    m_active = std::move(m_pending);
  }
  old.reset();
}

void CFrameExec::Abort() noexcept
{
  std::shared_ptr<Core> drop;
  {
    CSRWExclusiveLock lock(m_lock);
    drop = std::move(m_pending);
  }
  drop.reset();
}

TexResult CFrameExec::Run(CInteropResource& src,
  const D12FrameFormat& format, uint64_t captureTime, uint64_t postStart,
  FrameDamage damage, const RECT * rects, unsigned count) noexcept
{
  std::shared_ptr<Core> core;
  {
    CSRWSharedLock lock(m_lock);
    core = m_active;
  }
  if (!core || !core->live)
    return TexResult::OK;
  return core->Run(src, format, NextSerial(), captureTime, postStart,
    damage, rects, count);
}
