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
#include <atomic>
#include <stdint.h>

#include "capture/CFrameScheduler.h"
#include "d3d/CInteropResource.h"
#include "transport/PreparedFrameBuffer.h"

struct CD3D12Device;

using namespace Microsoft::WRL;

class CFrameBufferResource
{
  private:
    FrameToken                m_token             = {};
    uint8_t                 * m_base              = nullptr;
    size_t                    m_size              = 0;
    size_t                    m_capacity          = 0;
    uint64_t                  m_heapOffset        = 0;
    size_t                    m_frameSize         = 0;
    uint64_t                  m_captureTime       = 0;
    uint64_t                  m_postProcessStart  = 0;
    uint64_t                  m_copyStart         = 0;

    CFrameScheduler::Schedule m_schedule          = {};

    unsigned                  m_timingEffectIndex = 0;
    uint64_t                  m_timingToken       = 0;
    bool                      m_fullCopy          = false;
    bool                      m_direct            = false;
    RECT                      m_copyDirtyRects[LG_MAX_DIRTY_RECTS * 2] = {};
    unsigned                  m_nbCopyDirtyRects  = 0;
    unsigned                  m_copyPitch         = 0;
    unsigned                  m_copyBytesPerPixel = 0;
    unsigned                  m_candidateIndex    = 0;
    std::atomic<bool>         m_completionHandled = false;
    ComPtr<ID3D12Resource>    m_res;
    void                    * m_map               = nullptr;

  public:
    bool Init(CD3D12Device * dx12, const FrameToken& token, uint8_t * base,
      uint64_t heapOffset, size_t size, size_t maxFrameSize, bool direct);
    void Reset();

    const FrameToken& GetToken() const { return m_token;     }
    size_t            GetFrameSize()   { return m_frameSize; }
    void *            GetMap()         { return m_map;       }

    void SetTiming(uint64_t captureTime, uint64_t postProcessStart,
      uint64_t copyStart)
    {
      m_captureTime      = captureTime;
      m_postProcessStart = postProcessStart;
      m_copyStart        = copyStart;
    }
    uint64_t GetCaptureTime     () const { return m_captureTime;      }
    uint64_t GetPostProcessStart() const { return m_postProcessStart; }
    uint64_t GetCopyStart       () const { return m_copyStart;        }
    void SetSchedule(const CFrameScheduler::Schedule& schedule)
    {
      m_schedule = schedule;
    }
    const CFrameScheduler::Schedule& GetSchedule() const
    {
      return m_schedule;
    }

    void SetPostProcessSample(
      unsigned effectIndex, uint64_t token, bool fullCopy)
    {
      m_timingEffectIndex = effectIndex;
      m_timingToken       = token;
      m_fullCopy          = fullCopy;
    }
    unsigned GetTimingEffectIndex() const { return m_timingEffectIndex; }
    uint64_t GetTimingToken      () const { return m_timingToken;       }
    bool     IsFullCopy          () const { return m_fullCopy;          }
    void SetCopyDamage(const RECT dirtyRects[], unsigned nbDirtyRects,
      bool fullCopy, unsigned pitch, unsigned bytesPerPixel);
    const RECT * GetCopyDirtyRects() const { return m_copyDirtyRects;    }
    unsigned GetCopyDirtyRectCount() const { return m_nbCopyDirtyRects; }
    unsigned GetCopyPitch         () const { return m_copyPitch;        }
    unsigned GetCopyBytesPerPixel () const
    {
      return m_copyBytesPerPixel;
    }
    void ResetCompletion()
    {
      m_completionHandled.store(false, std::memory_order_release);
    }
    void MarkCompletion()
    {
      m_completionHandled.store(true, std::memory_order_release);
    }
    bool CompletionHandled() const
    {
      return m_completionHandled.load(std::memory_order_acquire);
    }
    void     SetCandidateIndex(unsigned index) { m_candidateIndex = index; }
    unsigned GetCandidateIndex() const         { return m_candidateIndex; }

    ComPtr<ID3D12Resource> Get() { return m_res; }
};
