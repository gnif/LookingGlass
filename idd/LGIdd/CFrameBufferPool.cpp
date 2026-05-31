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

#include "CFrameBufferPool.h"
#include "CSwapChainProcessor.h"

#include <stdint.h>

void CFrameBufferPool::Init(CSwapChainProcessor * swapChain)
{
  m_swapChain = swapChain;
}

void CFrameBufferPool::Reset()
{
  for (int i = 0; i < ARRAYSIZE(m_buffers); ++i)
    m_buffers[i].Reset();
}

CFrameBufferResource * CFrameBufferPool::Get(
  const CIndirectDeviceContext::PreparedFrameBuffer& buffer,
  size_t minSize)
{
  if (buffer.frameIndex > ARRAYSIZE(m_buffers) - 1)
    return nullptr;

  CFrameBufferResource* fbr = &m_buffers[buffer.frameIndex];
  if (!fbr->IsValid() || fbr->GetBase() != buffer.mem || fbr->GetSize() < minSize)
  {
    fbr->Reset();
    if (!fbr->Init(m_swapChain, buffer.frameIndex, buffer.mem, minSize))
      return nullptr;
  }

  return fbr;
}
