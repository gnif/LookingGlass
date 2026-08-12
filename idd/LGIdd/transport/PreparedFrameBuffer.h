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

#include <stddef.h>
#include <stdint.h>

enum : unsigned
{
  FRAME_MAX_SINKS       = 8,
  FRAME_BATCHES         = 3,
  FRAME_SINK_BUFFERS    = 3,
  FRAME_BUFFER_RESOURCES = FRAME_MAX_SINKS * FRAME_SINK_BUFFERS,
};

struct FrameToken
{
  uint32_t backend = 0;
  uint32_t epoch  = 0;
  uint32_t slot   = 0;
  uint64_t serial = 0;
};

enum class FrameFill
{
  READY,
  PENDING,
  REJECTED,
};

enum class FrameDone
{
  READY,
  FAILED,
  SUPERSEDED,
};

struct PreparedFrameBuffer
{
  FrameToken token;
  unsigned   resourceSlot = 0;
  uint8_t  * mem          = nullptr;
  uint64_t   heapOffset   = 0;
  size_t     capacity     = 0;
  bool       direct       = false;
  bool       fullCopy     = false;
};

struct FrameBatchToken
{
  uint32_t slot   = 0;
  uint64_t serial = 0;
};

struct PreparedFrameBatch
{
  FrameBatchToken     token;
  PreparedFrameBuffer targets[FRAME_MAX_SINKS] = {};
  unsigned            count = 0;
};

struct SinkTarget
{
  unsigned  slot       = 0;
  uint8_t * mem        = nullptr;
  uint64_t  heapOffset = 0;
  size_t    capacity   = 0;
  bool      fullCopy   = false;
};
