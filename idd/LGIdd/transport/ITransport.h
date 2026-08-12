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
#include "transport/FrameCaps.h"
#include "transport/TransportConfig.h"

#include <memory>
#include <stddef.h>
#include <stdint.h>

class IControlSink;
class IFrameSink;
class IInputSource;

struct SourceKey
{
  BackendId backend    = 0;
  uint32_t  epoch      = 0;
  uint32_t  client     = 0;
  uint32_t  generation = 0;
};

class ITransportEvents
{
public:
  virtual ~ITransportEvents() = default;

  virtual void OnSetCursorPos(
    const SourceKey& source, int32_t x, int32_t y) = 0;
  virtual void OnSetResolution(
    const SourceKey& source, uint32_t width, uint32_t height) = 0;
  virtual void OnRecoveryRequest(const SourceKey& source,
    uint64_t session, uint32_t serial, bool active) = 0;
};

class ITransport
{
public:
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
  // Process performs a bounded, non-blocking drain. The events reference is
  // valid only for the duration of this call and must not be retained.
  virtual ProcessResult Process(ITransportEvents& events) = 0;
  virtual void Stop() = 0;
  virtual void SyncRecovery() {}
  virtual void RecoveryStatus(
    uint64_t, uint32_t, bool, Recovery, uint32_t) {}

  // Frame capabilities describe the configured instance, not transient
  // runtime state. CanUseMode answers are immutable for the returned
  // object's lifetime, and recreating an instance from the same descriptor
  // must provide the same capability contract.
  virtual std::shared_ptr<const FrameCaps> GetFrameCaps() const
  {
    return std::shared_ptr<const FrameCaps>();
  }
  virtual DirectFrameBufferMemory GetDirectMemory() const = 0;

  // Component pointers are fixed after Setup and remain valid until Stop.
  virtual IFrameSink * FrameSink() { return nullptr; }
  virtual IControlSink * Control() { return nullptr; }
  virtual IInputSource * Input() { return nullptr; }
};
