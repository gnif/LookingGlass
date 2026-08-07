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

#include "d3d/CD3D12Device.h"
#include "capture/CFrameBufferPool.h"
#include "d3d/CInteropResource.h"
#include "postprocess/CPostProcessor.h"

#include <Windows.h>
#include <memory>

using namespace Microsoft::WRL;

class CFrameTransport;

struct FrameSubmission
{
  CInteropResource * source;
  D12FrameFormat     sourceFormat;
  uint64_t           captureTime;
  uint64_t           postProcessStart;
  unsigned           timingEffectIndex;
  uint64_t           timingToken;
  bool               noImageUpdate;
};

class CFrameProcessor
{
protected:
  CFrameTransport               * m_transport;
  std::shared_ptr<CD3D12Device>   m_dx12;
  CPostProcessor                * m_postProcessors;
  SRWLOCK                       * m_pipelineLock;
  HANDLE                          m_terminateEvent;
  CFrameBufferPool                m_frameBuffers;
  Wrappers::Event                 m_readyEvent;

  mutable SRWLOCK m_damageLock                           = SRWLOCK_INIT;
  RECT            m_previousDamage[LG_MAX_DIRTY_RECTS]   = {};
  unsigned        m_previousDamageCount                  = 0;
  RECT            m_pendingDamage[LG_MAX_DIRTY_RECTS]    = {};
  unsigned        m_pendingDamageCount                   = 0;
  bool            m_hasPendingDamage                     = true;

  bool HasPendingDamage() const;
  bool TakePendingDamage(RECT dirtyRects[], unsigned * count);
  void RestorePendingDamage(
    const RECT dirtyRects[], unsigned count, bool hasDamage);
  void CommitDamage(const RECT dirtyRects[], unsigned count);
  void GetPreviousDamage(RECT dirtyRects[], unsigned * count) const;
  virtual void AccumulateDamageLocked(
    const RECT dirtyRects[], unsigned count);
  virtual void SetFullDamageLocked();

public:
  CFrameProcessor(CFrameTransport * transport,
    std::shared_ptr<CD3D12Device> dx12,
    CPostProcessor postProcessors[LGMP_Q_FRAME_LEN],
    SRWLOCK * pipelineLock, HANDLE terminateEvent);
  virtual ~CFrameProcessor() = default;

  virtual bool IsValid() const;
  virtual bool Submit(const FrameSubmission& submission) = 0;
  virtual bool HasReadyFrame() const                     = 0;
  virtual bool Publish(const CFrameScheduler::Schedule& schedule,
    bool periodic, uint64_t publishStart)                = 0;
  virtual bool UsesCadence() const                       = 0;
  virtual void Reset();
  virtual void Invalidate();
  virtual void ResetPipeline();
  void AccumulateDamage(const RECT dirtyRects[], unsigned count);
  void SetFullDamage();

  HANDLE GetReadyEvent() const { return m_readyEvent.Get(); }
};

std::unique_ptr<CFrameProcessor> CreateFrameProcessor(
  bool software, CFrameTransport * transport,
  std::shared_ptr<CD3D12Device> dx12,
  CPostProcessor postProcessors[LGMP_Q_FRAME_LEN],
  SRWLOCK * pipelineLock, HANDLE terminateEvent);
