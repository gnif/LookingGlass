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
#include "capture/CFrameScheduler.h"
#include "transport/PreparedFrameBuffer.h"

#include <Windows.h>
#include <stddef.h>
#include <stdint.h>

struct FramePlanTarget
{
  uint32_t                  sink    = 0;
  uint32_t                  backend = 0;
  uint32_t                  epoch   = 0;
  CFrameScheduler::Schedule schedule = {};
  CFrameScheduler::Schedule commitSchedule = {};
  bool                      periodic = false;
  bool                      primary  = false;
};

struct FramePlan
{
  FramePlanTarget targets[FRAME_MAX_SINKS] = {};
  unsigned        count                    = 0;
  uint64_t        nextWake                 = 0;
  bool            progressed               = false;
};

class IFrameTransport
{
public:
  virtual ~IFrameTransport() = default;

  virtual size_t GetMaxFrameSize() const = 0;
  virtual uint64_t NextContentSerial() = 0;
  virtual void FrameProductReady(uint64_t contentSerial) = 0;
  virtual bool NeedsFrame() const = 0;
  virtual bool GetFramePlan(
    uint64_t now, bool productReady, FramePlan& plan) = 0;
  virtual bool GetImmediateFramePlan(
    uint64_t now, FramePlan& plan) = 0;
  virtual void MissFramePlan(const FramePlan& plan, uint64_t now) = 0;

  virtual bool PrepareFrameBatch(const FramePlan& plan,
    uint64_t contentSerial, unsigned pitch, size_t frameSize,
    const D12FrameFormat& srcFormat, const D12FrameFormat& dstFormat,
    const RECT * dirtyRects, unsigned nbDirtyRects,
    bool allowReadyReplacement, PreparedFrameBatch& batch) = 0;
  virtual uint32_t PublishFrameBatch(const FrameBatchToken& token) = 0;
  virtual void CommitFrameBatch(const FrameBatchToken& token) = 0;
  virtual void AbortFrameBatch(const FrameBatchToken& token) = 0;
  virtual void FailFrameBatch(const FrameBatchToken& token) = 0;

  virtual void WriteFrameTarget(const FrameBatchToken& token,
    unsigned target, void * src, size_t offset, size_t len,
    bool setWritePos) const = 0;
  virtual void WriteFrameTargetRows(const FrameBatchToken& token,
    unsigned target, void * src, size_t offset, size_t rowBytes,
    size_t pitch, unsigned rows) const = 0;
  virtual void FinalizeFrameTarget(
    const FrameBatchToken& token, unsigned target) const = 0;
  virtual void SetFrameTargetTiming(const FrameBatchToken& token,
    unsigned target, uint64_t captureTime, uint64_t postProcessTime,
    uint64_t copyTime, uint64_t readyTime, uint64_t holdTime,
    uint64_t completedAt) = 0;
  virtual void TryRecordFrameTiming(const FrameBatchToken& token,
    unsigned target, uint64_t duration) = 0;
  virtual void CompleteFrameTarget(const FrameBatchToken& token,
    unsigned target, bool succeeded) = 0;

  virtual void ObserveFrame(uint64_t now) = 0;
  virtual void ForceFrame() = 0;
  virtual void FrameSuperseded() = 0;
  virtual HANDLE GetFrameScheduleEvent() const = 0;
};
