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

#include "transport/DirectFrameBufferMemory.h"
#include "transport/FrameMemoryLimits.h"

#include <stddef.h>
#include <stdint.h>

class IControlTransport;
class IFrameTransport;
class IInputTransport;

class ITransportEvents
{
public:
  virtual ~ITransportEvents() = default;

  virtual void OnSetCursorPos(int32_t x, int32_t y) = 0;
  virtual void OnSetResolution(uint32_t width, uint32_t height) = 0;
  virtual void OnRecoveryRequest(
    uint64_t session, uint32_t serial, bool active) = 0;
};

class ITransport
{
public:
  using BackendId = uint32_t;

  enum class OpenResult
  {
    SUCCESS,
    RETRY,
    FAILURE,
  };

  enum class ProcessResult
  {
    OK,
    RETRY,
    FAILURE,
  };

  enum class Recovery
  {
    NORMAL,
    ACTIVE,
    FAILED,
  };

  virtual ~ITransport() = default;

  virtual OpenResult Open() = 0;
  virtual bool Initialize() = 0;
  virtual bool Setup(size_t alignment) = 0;
  virtual ProcessResult Process(ITransportEvents& events) = 0;
  virtual void Stop() = 0;
  virtual void SyncRecovery() {}
  virtual void RecoveryStatus(
    uint64_t, uint32_t, bool, Recovery, uint32_t) {}

  virtual FrameMemoryLimits GetMemoryLimits() const = 0;
  virtual DirectFrameBufferMemory GetDirectMemory() const = 0;

  virtual IFrameTransport& Frames() = 0;
  virtual IControlTransport& Control() = 0;
  virtual IInputTransport * Input() { return nullptr; }
};
