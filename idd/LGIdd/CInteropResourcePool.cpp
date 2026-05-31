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

#include "CInteropResourcePool.h"
#include "CDebug.h"

void CInteropResourcePool::Init(std::shared_ptr<CD3D11Device> dx11Device, std::shared_ptr<CD3D12Device> dx12Device)
{
  Reset();
  m_dx11Device = dx11Device;
  m_dx12Device = dx12Device;
}

void CInteropResourcePool::Reset()
{
  for (unsigned i = 0; i < POOL_SIZE; ++i)
    m_pool[i].Reset();
  m_dx11Device.reset();
  m_dx12Device.reset();
}

CInteropResource* CInteropResourcePool::Get(ComPtr<ID3D11Texture2D> srcTex)
{
  CInteropResource * res;
  unsigned freeSlot = POOL_SIZE;
  for (unsigned i = 0; i < POOL_SIZE; ++i)
  {
    res = &m_pool[i];
    if (!res->IsReady())
    {
      freeSlot = min(freeSlot, i);
      continue;
    }

    if (res->Compare(srcTex))
      return res;
  }

  if (freeSlot == POOL_SIZE)
  {
    DEBUG_ERROR("Interop Resouce Pool Full");
    return nullptr;
  }

  res = &m_pool[freeSlot];
  if (!res->Init(m_dx11Device, m_dx12Device, srcTex))
    return nullptr;

  return res;
}