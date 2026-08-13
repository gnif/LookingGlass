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

#include "capture/CFrameGraph.h"

#include "CDebug.h"

#include <cstring>

bool Frame::Same(const GraphCfg& left, const GraphCfg& right)
{
  return
    left.mode      == right.mode                         &&
    Frame::Same(left.adapter, right.adapter)             &&
    left.srcWidth  == right.srcWidth                     &&
    left.srcHeight == right.srcHeight                    &&
    Frame::Same(left.src, right.src)                     &&
    left.transform == right.transform                    &&
    left.width     == right.width                        &&
    left.height    == right.height                       &&
    Frame::Same(left.checkpoint, right.checkpoint);
}

bool Frame::Valid(FrameDamage damage, const RECT * rects, unsigned count,
  unsigned width, unsigned height)
{
  switch (damage)
  {
    case FrameDamage::NONE:
    case FrameDamage::FULL:
      return count == 0;

    case FrameDamage::RECTS:
      if (!rects || !count || count > FRAME_DAMAGE_MAX)
        return false;
      for (unsigned i = 0; i < count; ++i)
      {
        const RECT& rect = rects[i];
        if (rect.left                               < 0            ||
            rect.top                                < 0            ||
            rect.left                               >= rect.right  ||
            rect.top                                >= rect.bottom ||
            static_cast<unsigned long>(rect.right)  > width        ||
            static_cast<unsigned long>(rect.bottom) > height)
          return false;
      }
      return true;
  }
  return false;
}

static const char * OpName(FrameOp op)
{
  switch (op)
  {
    case FrameOp::SRC:   return "src";
    case FrameOp::CAL:   return "cal";
    case FrameOp::LUT:   return "lut";
    case FrameOp::SCALE: return "scale";
    case FrameOp::SDR:   return "sdr";
    case FrameOp::SCRGB: return "scrgb";
    case FrameOp::HDR10: return "hdr10";
  }
  return "?";
}

static const char * PixelName(FramePixel pixel)
{
  switch (pixel)
  {
    case FramePixel::BGRA8:   return "bgra8";
    case FramePixel::RGBA8:   return "rgba8";
    case FramePixel::RGB10A2: return "rgb10";
    case FramePixel::RGBA16F: return "rgba16f";
  }
  return "?";
}

static const char * SignalName(FrameSignal signal)
{
  switch (signal)
  {
    case FrameSignal::SRGB:         return "srgb";
    case FrameSignal::PQ_BT2020:    return "pq";
    case FrameSignal::SCRGB_LINEAR: return "scrgb";
  }
  return "?";
}

static const char * StorageName(FrameStorage storage)
{
  switch (storage)
  {
    case FrameStorage::D3D11_TEXTURE: return "d3d11";
    case FrameStorage::D3D12_TEXTURE: return "d3d12";
    default:                          return "?";
  }
}

static void Branch(char * result, const bool * ancestry,
  unsigned depth, bool last)
{
  char * current = result;
  for (unsigned i = 0; i < depth; ++i)
  {
    const char * segment = ancestry[i] ? "   " : "|  ";
    memcpy(current, segment, 3);
    current += 3;
  }
  memcpy(current, last ? "`- " : "+- ", 3);
  current[3] = 0;
}

void CFrameGraph::Reset()
{
  m_cfg        = GraphCfg {};
  m_generation = 0;
  m_nodeCount  = 0;
  m_leafCount  = 0;
  m_begun      = false;
  m_sealed     = false;
}

bool CFrameGraph::Begin(const GraphCfg& cfg)
{
  Reset();
  if (!cfg.srcWidth || !cfg.srcHeight || !cfg.width || !cfg.height ||
      !Frame::Valid(cfg.mode) ||
      cfg.src.storage != FrameStorage::D3D12_TEXTURE ||
      cfg.checkpoint.storage != FrameStorage::D3D12_TEXTURE ||
      !Frame::Valid(cfg.src) || !Frame::Valid(cfg.checkpoint) ||
      !Frame::Can(cfg.src.signal, cfg.checkpoint.signal))
    return false;

  m_cfg = cfg;
  GraphNode& source = m_nodes[m_nodeCount++];
  source         = GraphNode {};
  source.op      = FrameOp::SRC;
  source.parent  = FRAME_GRAPH_ROOT;
  source.width   = cfg.srcWidth;
  source.height  = cfg.srcHeight;
  source.profile = cfg.src;
  m_begun         = true;
  return true;
}

bool CFrameGraph::Can(const FrameCfg& cfg) const
{
  if (!m_begun || m_sealed || !cfg.width || !cfg.height ||
      cfg.width != m_cfg.width || cfg.height != m_cfg.height ||
      cfg.mode != m_cfg.mode ||
      !Frame::Same(cfg.adapter, m_cfg.adapter) ||
      !Frame::Valid(cfg.profile))
    return false;
  return Frame::Can(m_cfg.src.signal, cfg.profile.signal);
}

unsigned CFrameGraph::Find(FrameOp op, unsigned parent,
  unsigned width, unsigned height,
  const FrameProfile& profile) const
{
  for (unsigned i = 1; i < m_nodeCount; ++i)
    if (m_nodes[i].op == op && m_nodes[i].parent == parent &&
        m_nodes[i].width == width && m_nodes[i].height == height &&
        Frame::Same(m_nodes[i].profile, profile))
      return i;
  return FRAME_GRAPH_ROOT;
}

unsigned CFrameGraph::AddNode(FrameOp op, unsigned parent,
  unsigned width, unsigned height,
  const FrameProfile& profile)
{
  const unsigned found = Find(op, parent, width, height, profile);
  if (found != FRAME_GRAPH_ROOT)
    return found;
  if (m_nodeCount == FRAME_GRAPH_MAX_NODES)
    return FRAME_GRAPH_ROOT;

  const unsigned index = m_nodeCount++;
  GraphNode& node = m_nodes[index];
  node         = GraphNode {};
  node.op      = op;
  node.parent  = parent;
  node.width   = width;
  node.height  = height;
  node.profile = profile;
  return index;
}

unsigned CFrameGraph::Checkpoint(const FrameProfile& requested)
{
  const FrameProfile profile =
    Frame::Store(requested, FrameStorage::D3D12_TEXTURE);
  FrameProfile current = m_cfg.src;
  unsigned parent = 0;

  const D12ColorTransform * transform = m_cfg.transform.get();
  if (transform && transform->matrixEnabled)
  {
    // The common matrix stage preserves the source signal. A transfer-domain
    // LUT is applied after each branch has selected scRGB or HDR10.
    if (current.signal == FrameSignal::SRGB &&
        current.pixel == FramePixel::BGRA8)
      current.pixel = FramePixel::RGBA8;
    parent = AddNode(FrameOp::CAL, 0,
      m_cfg.srcWidth, m_cfg.srcHeight, current);
    if (parent == FRAME_GRAPH_ROOT)
      return FRAME_GRAPH_ROOT;
  }

  // Without calibration the established path filters linear scRGB before
  // HDR10 encoding. This scale node is also shared by scRGB and HDR10 leaves.
  const bool earlyScale = !transform &&
    (m_cfg.srcWidth != m_cfg.width || m_cfg.srcHeight != m_cfg.height);
  if (earlyScale)
  {
    parent = AddNode(FrameOp::SCALE, parent,
      m_cfg.width, m_cfg.height, current);
    if (parent == FRAME_GRAPH_ROOT)
      return FRAME_GRAPH_ROOT;
  }

  switch (profile.signal)
  {
    case FrameSignal::SRGB:
      break;

    case FrameSignal::SCRGB_LINEAR:
      break;

    case FrameSignal::PQ_BT2020:
      if (m_cfg.src.signal == FrameSignal::SCRGB_LINEAR)
      {
        const unsigned width  = earlyScale ? m_cfg.width : m_cfg.srcWidth;
        const unsigned height = earlyScale ? m_cfg.height : m_cfg.srcHeight;
        parent = AddNode(FrameOp::HDR10, parent,
          width, height, profile);
        if (parent == FRAME_GRAPH_ROOT)
          return FRAME_GRAPH_ROOT;
        current = profile;
      }
      break;

    default:
      return FRAME_GRAPH_ROOT;
  }

  if (transform && transform->lutEnabled)
  {
    if (current.signal == FrameSignal::SRGB &&
        current.pixel == FramePixel::BGRA8)
      current.pixel = FramePixel::RGBA8;
    parent = AddNode(FrameOp::LUT, parent,
      m_cfg.srcWidth, m_cfg.srcHeight, current);
    if (parent == FRAME_GRAPH_ROOT)
      return FRAME_GRAPH_ROOT;
  }

  if (!earlyScale &&
      (m_cfg.srcWidth != m_cfg.width || m_cfg.srcHeight != m_cfg.height))
  {
    parent = AddNode(FrameOp::SCALE, parent,
      m_cfg.width, m_cfg.height, current);
    if (parent == FRAME_GRAPH_ROOT)
      return FRAME_GRAPH_ROOT;
  }

  if (parent != 0 && Frame::Same(current, profile))
    return parent;

  FrameOp op;
  switch (profile.signal)
  {
    case FrameSignal::SRGB:
      op = FrameOp::SDR;
      break;
    case FrameSignal::SCRGB_LINEAR:
      op = FrameOp::SCRGB;
      break;
    case FrameSignal::PQ_BT2020:
      op = FrameOp::HDR10;
      break;
    default:
      return FRAME_GRAPH_ROOT;
  }
  return AddNode(op, parent, m_cfg.width, m_cfg.height, profile);
}

bool CFrameGraph::Add(BackendId id, uint32_t epoch, bool required,
  bool primary, bool tex, bool continuous, const FrameCfg& cfg)
{
  if (!id || !epoch || !Can(cfg) ||
      m_leafCount == TRANSPORT_MAX_INSTANCES)
    return false;

  for (unsigned i = 0; i < m_leafCount; ++i)
    if (m_leaves[i].id == id && m_leaves[i].epoch == epoch)
      return false;

  const CFrameGraph before = *this;
  const unsigned node = Checkpoint(cfg.profile);
  if (node == FRAME_GRAPH_ROOT)
  {
    *this = before;
    return false;
  }

  GraphLeaf& leaf = m_leaves[m_leafCount++];
  leaf            = GraphLeaf {};
  leaf.id         = id;
  leaf.epoch      = epoch;
  leaf.node       = node;
  leaf.required   = required;
  leaf.primary    = primary;
  leaf.tex        = tex;
  leaf.continuous = continuous;
  leaf.cfg        = cfg;

  for (unsigned current = node;
       current != FRAME_GRAPH_ROOT; current = m_nodes[current].parent)
  {
    ++m_nodes[current].refs;
    if (tex)
      ++m_nodes[current].texRefs;
  }
  return true;
}

bool CFrameGraph::Seal()
{
  if (!m_begun || m_sealed || !m_leafCount || !m_nodeCount ||
      !m_nodes[0].refs)
    return false;

  for (unsigned i = 0; i < m_leafCount; ++i)
    if (!m_leaves[i].id || !m_leaves[i].epoch ||
        !m_leaves[i].node || m_leaves[i].node >= m_nodeCount)
      return false;
  for (unsigned i = 0; i < m_nodeCount; ++i)
    if (!m_nodes[i].refs || m_nodes[i].texRefs > m_nodes[i].refs)
      return false;

  m_sealed = true;
  return true;
}

bool CFrameGraph::Stamp(uint64_t generation)
{
  if (!m_sealed || m_generation || !generation ||
      !m_leafCount || !m_nodeCount)
    return false;
  m_generation = generation;
  return true;
}

void CFrameGraph::LogNode(unsigned node, const GraphRouteName * routes,
  unsigned routeCount, bool * ancestry, unsigned depth, bool last) const
{
  char branch[FRAME_GRAPH_MAX_NODES * 3 + 4];
  Branch(branch, ancestry, depth, last);

  const GraphNode& item = m_nodes[node];
  switch (item.op)
  {
    case FrameOp::SRC:
      DEBUG_INFO("%ssrc %s/%s", branch,
        PixelName(item.profile.pixel), SignalName(item.profile.signal));
      break;

    case FrameOp::SDR:
    case FrameOp::SCRGB:
      DEBUG_INFO("%snative %s", branch, PixelName(item.profile.pixel));
      break;

    case FrameOp::SCALE:
      DEBUG_INFO("%sscale %ux%u %s/%s", branch,
        item.width, item.height, PixelName(item.profile.pixel),
        SignalName(item.profile.signal));
      break;

    default:
      DEBUG_INFO("%s%s %s/%s", branch, OpName(item.op),
        PixelName(item.profile.pixel), SignalName(item.profile.signal));
      break;
  }

  unsigned children = 0;
  for (unsigned child = 1; child < m_nodeCount; ++child)
    if (m_nodes[child].refs && m_nodes[child].parent == node)
      ++children;
  for (unsigned leaf = 0; leaf < m_leafCount; ++leaf)
    if (m_leaves[leaf].node == node)
      ++children;

  ancestry[depth] = last;
  unsigned emitted = 0;
  for (unsigned child = 1; child < m_nodeCount; ++child)
    if (m_nodes[child].refs && m_nodes[child].parent == node)
      LogNode(child, routes, routeCount, ancestry, depth + 1,
        ++emitted == children);

  for (unsigned leaf = 0; leaf < m_leafCount; ++leaf)
  {
    const GraphLeaf& endpoint = m_leaves[leaf];
    if (endpoint.node != node)
      continue;

    const wchar_t * name = L"transport";
    for (unsigned route = 0; routes && route < routeCount; ++route)
      if (routes[route].id    == endpoint.id    &&
          routes[route].epoch == endpoint.epoch &&
          routes[route].name)
      {
        name = routes[route].name;
        break;
      }

    Branch(branch, ancestry, depth + 1, ++emitted == children);
    DEBUG_INFO("%s%ls:%u %s", branch, name, endpoint.id,
      StorageName(endpoint.cfg.profile.storage));
  }
}

void CFrameGraph::Log(const GraphRouteName * routes,
  unsigned routeCount) const
{
  if (!Ready() || routeCount > TRANSPORT_MAX_INSTANCES ||
      (routeCount && !routes))
    return;

  const unsigned long long generation =
    static_cast<unsigned long long>(m_generation);
  const char * mode = m_cfg.mode == GpuMode::HARDWARE ? "hw" : "sw";
  if (m_cfg.srcWidth == m_cfg.width && m_cfg.srcHeight == m_cfg.height)
    DEBUG_INFO("Frame graph g=%llu mode=%s %ux%u", generation, mode,
      m_cfg.srcWidth, m_cfg.srcHeight);
  else
    DEBUG_INFO("Frame graph g=%llu mode=%s %ux%u -> %ux%u",
      generation, mode, m_cfg.srcWidth, m_cfg.srcHeight,
      m_cfg.width, m_cfg.height);
  bool ancestry[FRAME_GRAPH_MAX_NODES] = {};
  LogNode(0, routes, routeCount, ancestry, 0, true);
}

bool CFrameGraph::Same(const GraphCfg& cfg) const
{
  return Ready() && Frame::Same(m_cfg, cfg);
}

const GraphLeaf * CFrameGraph::FindLeaf(BackendId id, uint32_t epoch,
  bool tex, const FrameCfg& frame) const
{
  if (!Ready())
    return nullptr;
  for (unsigned i = 0; i < m_leafCount; ++i)
  {
    const GraphLeaf& leaf = m_leaves[i];
    if (leaf.id    == id    &&
        leaf.epoch == epoch &&
        leaf.tex   == tex   &&
        Frame::Same(leaf.cfg, frame))
      return &leaf;
  }
  return nullptr;
}

bool CFrameGraph::Need(FrameOp op) const
{
  if (!Ready() || op == FrameOp::SRC)
    return false;
  for (unsigned i = 1; i < m_nodeCount; ++i)
    if (m_nodes[i].op == op && m_nodes[i].refs)
      return true;
  return false;
}

bool CFrameGraph::Want(FrameSignal signal) const
{
  if (!Ready())
    return false;
  for (unsigned i = 0; i < m_leafCount; ++i)
    if (m_leaves[i].cfg.profile.signal == signal)
      return true;
  return false;
}

bool CFrameGraph::Shared(unsigned node) const
{
  if (!Ready() || !node || node >= m_nodeCount ||
      !m_nodes[node].texRefs)
    return false;
  for (unsigned i = 0; i < m_leafCount; ++i)
    if (m_leaves[i].tex                         &&
        m_leaves[i].node                == node &&
        m_leaves[i].cfg.profile.storage == FrameStorage::D3D11_TEXTURE)
      return true;
  return false;
}

bool CFrameGraph::Desc(unsigned nodeIndex,
  const FrameContentRef& content, FrameDesc& desc) const
{
  if (!Ready() || !content)
    return false;

  const D12FrameFormat& source = content->format;
  FrameProfile sourceProfile;
  const bool validProfile = D12::Profile(source,
    FrameStorage::D3D12_TEXTURE, sourceProfile);
  const auto effectiveTransform =
    D12::Transform(source.colorTransform);
  if (!validProfile || !Frame::Same(sourceProfile, m_cfg.src))
    return false;

  if (!nodeIndex                                               ||
      nodeIndex                    >= m_nodeCount              ||
      !content->serial                                         ||
      source.width                 != m_cfg.srcWidth           ||
      source.height                != m_cfg.srcHeight          ||
      source.dataWidth             != m_cfg.srcWidth           ||
      source.dataHeight            != m_cfg.srcHeight          ||
      source.pitch                 != 0                        ||
      source.desc.Dimension        !=
        D3D12_RESOURCE_DIMENSION_TEXTURE2D                     ||
      source.desc.Width            != m_cfg.srcWidth           ||
      source.desc.Height           != m_cfg.srcHeight          ||
      source.desc.DepthOrArraySize != 1                        ||
      source.desc.MipLevels        != 1                        ||
      source.desc.SampleDesc.Count != 1                        ||
      effectiveTransform           != m_cfg.transform          ||
      !Frame::Valid(content->damage, content->rects, content->count,
        m_cfg.srcWidth, m_cfg.srcHeight))
    return false;

  const GraphNode& node = m_nodes[nodeIndex];
  FrameDesc result;
  result.content = content;
  result.damage  = content->damage;
  result.count   = content->count;
  if (content->count)
    memcpy(result.rects, content->rects,
      content->count * sizeof(*content->rects));
  result.format  = content->format;

  // A scaled checkpoint has a different damage coordinate space. Until the
  // executor owns exact edge transforms, preserve correctness by making any
  // partial source damage a full node update.
  if (result.damage == FrameDamage::RECTS &&
      (node.width != m_cfg.srcWidth || node.height != m_cfg.srcHeight))
  {
    result.damage = FrameDamage::FULL;
    result.count  = 0;
  }

  D12FrameFormat& output = result.format;
  if (!D12::Set(output, node.profile, node.width, node.height))
    return false;
  // Calibration/LUT nodes have already consumed this transform.
  output.colorTransform.reset();

  desc = result;
  return true;
}

bool CFrameGraph::Desc(unsigned leaf, const FrameContentRef& content,
  LeafDesc& desc) const
{
  if (!Ready() || leaf >= m_leafCount)
    return false;

  const GraphLeaf& route = m_leaves[leaf];
  LeafDesc result;
  if (!Desc(route.node, content, result.frame))
    return false;

  result.id      = route.id;
  result.epoch   = route.epoch;
  result.node    = route.node;
  result.profile = route.cfg.profile;
  desc = result;
  return true;
}

const GraphNode * CFrameGraph::Nodes(unsigned& count) const
{
  count = Ready() ? m_nodeCount : 0;
  return count ? m_nodes : nullptr;
}

const GraphLeaf * CFrameGraph::Leaves(unsigned& count) const
{
  count = Ready() ? m_leafCount : 0;
  return count ? m_leaves : nullptr;
}
