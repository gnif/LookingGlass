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

#include "transport/lgmp/CLGMPFrameCaps.h"

#include <d3d12.h>
#include <limits>

namespace
{
  static const uint64_t FRAME_BYTES_PER_PIXEL = 4;

  bool AlignUp(uint64_t value, uint64_t alignment, uint64_t& result)
  {
    if (!alignment || (alignment & (alignment - 1)))
      return false;

    const uint64_t mask = alignment - 1;
    if (value > (std::numeric_limits<uint64_t>::max)() - mask)
      return false;
    result = (value + mask) & ~mask;
    return true;
  }

  bool CalculateFrameSize(
    const FrameMode& mode, uint64_t& frameSize)
  {
    frameSize = 0;
    if (!mode.width || !mode.height || !mode.refreshMilliHz)
      return false;

    uint64_t pitch;
    if (!AlignUp((uint64_t)mode.width * FRAME_BYTES_PER_PIXEL,
        D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, pitch) ||
        pitch > (std::numeric_limits<uint64_t>::max)() / mode.height)
      return false;

    frameSize = pitch * mode.height;
    return true;
  }

  uint32_t RecommendedSizeMiB(uint64_t requiredSize)
  {
    uint64_t sizeMiB = requiredSize / 1048576;
    if (requiredSize % 1048576)
      ++sizeMiB;

    uint64_t result = 1;
    while (result < sizeMiB && result <= UINT32_MAX / 2)
      result <<= 1;
    return result < sizeMiB ? UINT32_MAX : (uint32_t)result;
  }
}

CLGMPFrameCaps::CLGMPFrameCaps(uint64_t capacity,
    uint64_t frameMemoryOffset, uint32_t bufferCount) :
  m_capacity(capacity),
  m_frameMemoryOffset(frameMemoryOffset),
  m_bufferCount(bufferCount)
{
}

bool CLGMPFrameCaps::CanUseMode(const FrameMode& mode,
  uint32_t * requiredSizeMiB) const
{
  if (requiredSizeMiB)
    *requiredSizeMiB = 0;

  const uint64_t alignment =
    D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  uint64_t frameSize;
  if (!m_frameMemoryOffset || !m_bufferCount ||
      !CalculateFrameSize(mode, frameSize) ||
      frameSize > (std::numeric_limits<uint64_t>::max)() - alignment)
    return false;

  uint64_t frameAllocationSize;
  uint64_t frameMemoryStart;
  if (!AlignUp(frameSize + alignment, alignment,
        frameAllocationSize) ||
      !AlignUp(m_frameMemoryOffset, alignment, frameMemoryStart) ||
      frameAllocationSize >
        ((std::numeric_limits<uint64_t>::max)() - frameMemoryStart) /
          m_bufferCount)
    return false;

  const uint64_t requiredSize = frameMemoryStart +
    frameAllocationSize * m_bufferCount;
  if (requiredSize <= m_capacity)
    return true;

  if (requiredSizeMiB)
    *requiredSizeMiB = RecommendedSizeMiB(requiredSize);
  return false;
}
