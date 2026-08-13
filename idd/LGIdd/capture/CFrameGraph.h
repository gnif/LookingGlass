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

#include "postprocess/D12FrameFormat.h"
#include "transport/FrameProfile.h"
#include "transport/TransportConfig.h"

#include <Windows.h>
#include <limits.h>
#include <stdint.h>

static const unsigned FRAME_DAMAGE_MAX = 256;
static const unsigned FRAME_GRAPH_ROOT = UINT_MAX;
static const unsigned FRAME_GRAPH_MAX_NODES =
  1 + 5 * TRANSPORT_MAX_INSTANCES;

enum class FrameOp : uint8_t
{
  SRC,
  CAL,
  LUT,
  SCALE,
  SDR,
  SCRGB,
  HDR10,
};

enum class FrameDamage : uint8_t
{
  NONE,
  RECTS,
  FULL,
};

struct GraphCfg
{
  GpuMode                                  mode      = GpuMode::HARDWARE;
  LUID                                     adapter   = {};
  unsigned                                 srcWidth  = 0;
  unsigned                                 srcHeight = 0;
  FrameProfile                             src;
  std::shared_ptr<const D12ColorTransform> transform;
  unsigned                                 width     = 0;
  unsigned                                 height    = 0;
  FrameProfile                             checkpoint;
};

namespace Frame
{
  bool Same(const GraphCfg& left, const GraphCfg& right);
  bool Valid(FrameDamage damage, const RECT * rects, unsigned count,
    unsigned width, unsigned height);
}

struct GraphNode
{
  FrameOp      op      = FrameOp::SRC;
  unsigned     parent  = FRAME_GRAPH_ROOT;
  unsigned     refs    = 0;
  unsigned     width   = 0;
  unsigned     height  = 0;
  FrameProfile profile;
};

struct GraphLeaf
{
  BackendId id       = 0;
  uint32_t  epoch    = 0;
  unsigned  node     = 0;
  bool      required = false;
  bool      primary  = false;
  FrameCfg  cfg;
};

// All products derived from one capture retain the same immutable content.
// It contains no acquired IddCx or Direct3D resource.
struct FrameContent
{
  uint64_t       serial                  = 0;
  uint64_t       captureTime             = 0;
  FrameDamage    damage                  = FrameDamage::NONE;
  RECT           rects[FRAME_DAMAGE_MAX] = {};
  unsigned       count                   = 0;
  D12FrameFormat format                  = {};
};

using FrameContentRef = std::shared_ptr<const FrameContent>;

// FrameDesc adds one graph node's representation and damage to the shared
// content identity. It remains resource-free.
struct FrameDesc
{
  FrameContentRef content;
  FrameDamage     damage                  = FrameDamage::NONE;
  RECT            rects[FRAME_DAMAGE_MAX] = {};
  unsigned        count                   = 0;
  D12FrameFormat  format                  = {};
};

struct LeafDesc
{
  BackendId    id     = 0;
  uint32_t     epoch  = 0;
  unsigned     node   = 0;
  FrameProfile profile;
  FrameDesc     frame = {};
};

class CFrameGraph
{
private:
  GraphCfg  m_cfg;
  GraphNode m_nodes[FRAME_GRAPH_MAX_NODES]    = {};
  GraphLeaf m_leaves[TRANSPORT_MAX_INSTANCES] = {};
  uint64_t  m_generation                      = 0;
  unsigned  m_nodeCount                       = 0;
  unsigned  m_leafCount                       = 0;
  bool      m_begun                           = false;
  bool      m_sealed                          = false;

  unsigned Find(FrameOp op, unsigned parent, unsigned width, unsigned height,
    const FrameProfile& profile) const;
  unsigned AddNode(FrameOp op, unsigned parent, unsigned width,
    unsigned height,
    const FrameProfile& profile);
  unsigned Checkpoint(const FrameProfile& profile);

public:
  void Reset();
  bool Begin(const GraphCfg& cfg);
  bool Can(const FrameCfg& cfg) const;
  bool Add(BackendId id, uint32_t epoch, bool required, bool primary,
    const FrameCfg& cfg);
  bool Seal();
  bool Stamp(uint64_t generation);
  bool Same(const GraphCfg& cfg) const;
  bool Want(FrameSignal signal) const;
  bool Need(FrameOp op) const;
  bool Shared(unsigned node) const;
  bool Desc(unsigned leaf, const FrameContentRef& content,
    LeafDesc& desc) const;

  const GraphCfg& Cfg() const { return m_cfg; }
  uint64_t Generation() const { return m_generation; }
  const GraphNode * Nodes(unsigned& count) const;
  const GraphLeaf * Leaves(unsigned& count) const;
};
