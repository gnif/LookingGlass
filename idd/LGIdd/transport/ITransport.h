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
#include "transport/FrameProfile.h"
#include "transport/TransportConfig.h"

#include <memory>
#include <stddef.h>
#include <stdint.h>

class IControlSink;
class IFrameSink;
class IInputSource;
class ITexSink;

struct SourceKey
{
  BackendId backend    = 0;
  uint32_t  epoch      = 0;
  uint32_t  client     = 0;
  uint32_t  generation = 0;
};

inline bool operator==(const SourceKey& left, const SourceKey& right)
{
  return
    left.backend    == right.backend    &&
    left.epoch      == right.epoch      &&
    left.client     == right.client     &&
    left.generation == right.generation;
}

inline bool operator!=(const SourceKey& left, const SourceKey& right)
{
  return !(left == right);
}

enum class InteractionResult
{
  ACCEPTED,
  BUSY,
  UNAVAILABLE,
  STALE,
  REJECTED,
  FAILED,
};

enum class RecoveryState
{
  NORMAL,
  ACTIVE,
  FAILED,
};

struct RecoveryAdmission
{
  bool          complete = false;
  RecoveryState state    = RecoveryState::FAILED;
  uint32_t      error    = 0;
};

struct RecoveryAction
{
  uint64_t route    = 0;
  uint64_t session  = 0;
  uint64_t deadline = 0;
  uint32_t serial   = 0;
  bool     active   = false;
};

class ITransportEvents
{
public:
  virtual ~ITransportEvents() = default;

  virtual InteractionResult OnSetCursorPos(
    const SourceKey& source, int32_t x, int32_t y) = 0;
  virtual InteractionResult OnSetResolution(
    const SourceKey& source, uint32_t width, uint32_t height) = 0;
  virtual RecoveryAdmission OnRecoveryRequest(const SourceKey& source,
    uint64_t session, uint32_t serial, bool active) = 0;
};

// Actions are implemented above the transport manager. Transport instances
// receive ITransportEvents only, so recovery work cannot bypass the manager's
// coordinator.
class ITransportActions
{
public:
  virtual ~ITransportActions() = default;

  virtual InteractionResult OnSetCursorPos(
    const SourceKey& source, int32_t x, int32_t y) = 0;
  virtual InteractionResult OnSetResolution(
    const SourceKey& source, uint32_t width, uint32_t height) = 0;
  virtual bool OnRecoveryAction(const RecoveryAction& action) = 0;
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

  using Recovery = RecoveryState;

  virtual ~ITransport() = default;

  virtual OpenResult Open() = 0;
  virtual bool Initialize() = 0;
  virtual bool Setup(size_t alignment) = 0;
  // Process performs a bounded, non-blocking drain. The events reference is
  // valid only for the duration of this call and must not be retained.
  virtual ProcessResult Process(ITransportEvents& events) = 0;
  virtual void Stop() = 0;
  virtual void SyncRecovery() {}
  virtual void RecoveryStatus(const SourceKey&, uint64_t, uint32_t,
    bool, Recovery, uint32_t) {}

  // Profiles is an immutable, ordered preference list of at most
  // FRAME_PROFILE_MAX entries whose pointer remains valid for the lifetime
  // of this instance.
  // Probe has no side effects. Prepare changes pending state only and is safe
  // while the active texture sink is admitting frames. Commit promotes it
  // without failure, while Abort preserves the active route. A texture
  // transport retains old-generation resources until every accepted lease
  // completes; Stop drains all accepted leases before destroying its sink.
  virtual const FrameProfile * Profiles(unsigned& count) const = 0;
  virtual CfgResult Probe(const FrameCfg& cfg) const = 0;
  virtual CfgResult Prepare(const FrameCfg& cfg) = 0;
  virtual void Commit() = 0;
  virtual void Abort() = 0;

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
  virtual ITexSink * TexSink() { return nullptr; }
  virtual IControlSink * Control() { return nullptr; }
  virtual IInputSource * Input() { return nullptr; }
};
