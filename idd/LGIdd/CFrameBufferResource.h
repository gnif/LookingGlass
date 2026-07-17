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

#include <Windows.h>
#include <wdf.h>
#include <wrl.h>
#include <d3d12.h>
#include <stdint.h>

class CSwapChainProcessor;

using namespace Microsoft::WRL;

class CFrameBufferResource
{
  private:
    bool                   m_valid      = false;
    unsigned               m_frameIndex = 0;
    uint8_t              * m_base       = nullptr;
    size_t                 m_size       = 0;
    size_t                 m_frameSize  = 0;
    ComPtr<ID3D12Resource> m_res;
    void                 * m_map        = nullptr;

  public:
    bool Init(CSwapChainProcessor * swapChain, unsigned frameIndex, uint8_t * base, size_t size);
    void Reset();

    bool      IsValid()       { return m_valid;      }
    unsigned  GetFrameIndex() { return m_frameIndex; }
    uint8_t * GetBase()       { return m_base;       }
    size_t    GetSize()       { return m_size;       }
    size_t    GetFrameSize()  { return m_frameSize;  }
    void *    GetMap()        { return m_map;        }

    ComPtr<ID3D12Resource> Get() { return m_res; }
};
