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

#include "CSRWLock.h"
#include "capture/CFrameScheduler.h"
#include "transport/IControlSink.h"
#include "transport/IInputSource.h"
#include "transport/ITransport.h"

#include <LGProtocol/NetworkServices.h>

#include <stdint.h>
#include <vector>

enum class NetworkEventType : uint8_t
{
  SET_CURSOR_POSITION,
  SET_RESOLUTION,
  SET_FRAME_SCHEDULE,
  RECOVERY_REQUEST,
};

struct NetworkEvent
{
  NetworkEventType     type      = NetworkEventType::SET_CURSOR_POSITION;
  SourceKey            source;
  FrameScheduleUpdate  schedule  = {};
  uint64_t             controlID = 0;
  int32_t              x         = 0;
  int32_t              y         = 0;
  uint32_t             width     = 0;
  uint32_t             height    = 0;
  uint64_t             session   = 0;
  uint32_t             serial    = 0;
  bool                 active    = false;
};

struct NetworkRecoveryStatus
{
  uint64_t      session = 0;
  uint32_t      serial  = 0;
  bool          active  = false;
  RecoveryState state   = RecoveryState::FAILED;
  uint32_t      error   = 0;
};

struct NetworkControlStatus
{
  uint64_t               controlID = 0;
  LGNetControlStatusCode status    = LG_NET_CONTROL_STATUS_ERROR;
  uint32_t               detail    = 0;
};

class CNetworkEventQueue
{
private:
  static constexpr unsigned CAPACITY = 128;

  CSRWLock     m_lock;
  NetworkEvent m_events[CAPACITY]  = {};
  unsigned     m_head              = 0;
  unsigned     m_count             = 0;

public:
  bool Push(const NetworkEvent& event);
  bool Take(NetworkEvent& event);
  void Reset();
};

class CNetworkInputSource final : public IInputSource
{
private:
  mutable CSRWLock m_lock;
  IInputTarget   * m_target = nullptr;

public:
  bool Start(IInputTarget& target) override;
  void Stop() override;

  InputTargetState GetState(const InputSourceId& source) const;
  bool WithAuthority(const InputSourceId& source,
    InputAuthorityAction action, void * opaque) const;
  InputResult Claim(const InputSourceId& source);
  InputResult Touch(const InputSourceId& source);
  InputResult Release(const InputSourceId& source, bool reset);
  InputResult SendMouseRelative(const InputSourceId& source,
    int32_t deltaX, int32_t deltaY, int32_t wheel, uint32_t buttons);
  InputResult SendMouseAbsolute(const InputSourceId& source,
    uint16_t x, uint16_t y, int32_t wheel, uint32_t buttons);
  InputResult SendKeyboard(const InputSourceId& source,
    uint8_t modifiers, const uint8_t * keys);
  InputResult Reset(const InputSourceId& source);
};

struct NetworkControlSnapshot
{
  IDARG_OUT_QUERY_HWCURSOR                cursor = {};
  std::vector<BYTE>                       cursorData;
  std::shared_ptr<const D12ColorTransform> transform;
  UINT                                    sdrWhiteLevel    = 0;
  uint64_t                                stateRevision    = 0;
  uint64_t                                positionRevision = 0;
  uint64_t                                shapeRevision    = 0;
  uint64_t                                colorRevision    = 0;
  bool                                    cursorValid      = false;
  bool                                    shapeValid       = false;
  bool                                    transformValid   = false;
};

class CNetworkControlSink final : public IControlSink
{
private:
  mutable CSRWLock        m_lock;
  IControlEvents        * m_events = nullptr;
  ControlToken            m_token;
  NetworkControlSnapshot  m_state;

public:
  void SetControlEvents(
    IControlEvents * events, const ControlToken& token) override;
  ControlResult SendCursor(const IDARG_OUT_QUERY_HWCURSOR& info,
    const BYTE * data, size_t size, UINT sdrWhiteLevel) override;
  ControlResult SetColorTransform(
    std::shared_ptr<const D12ColorTransform> transform) override;

  void RequestReplay();
  bool Snapshot(
    NetworkControlSnapshot& state, uint64_t knownRevision) const;
};

// A session is transport-independent state owned by the eventual QUIC
// provider. Decoded requests enter the bounded event queue or the existing
// input hub; encoded replies are drained from the bounded status queue.
class CNetworkSession final
{
private:
  static constexpr unsigned RECOVERY_CAPACITY = 16;
  static constexpr unsigned CONTROL_CAPACITY  = 128;

  CNetworkEventQueue&  m_events;
  CNetworkInputSource& m_input;
  InputSourceId        m_source;
  mutable CSRWLock     m_lifecycleLock;
  bool                 m_open = true;

  CSRWLock              m_recoveryLock;
  NetworkRecoveryStatus m_recovery[RECOVERY_CAPACITY]  = {};
  unsigned              m_recoveryHead                 = 0;
  unsigned              m_recoveryCount                = 0;

  CSRWLock             m_controlLock;
  NetworkControlStatus m_control[CONTROL_CAPACITY]  = {};
  unsigned             m_controlHead                = 0;
  unsigned             m_controlCount               = 0;

  SourceKey EventSource() const;

public:
  CNetworkSession(uint32_t client, uint32_t generation,
    CNetworkEventQueue& events, CNetworkInputSource& input);
  ~CNetworkSession();

  CNetworkSession(const CNetworkSession&) = delete;
  CNetworkSession& operator=(const CNetworkSession&) = delete;

  InputSourceId Source() const { return m_source; }
  bool IsOpen() const;
  void Close();

  bool SetCursorPosition(uint64_t controlID, int32_t x, int32_t y);
  bool SetResolution(
    uint64_t controlID, uint32_t width, uint32_t height);
  bool SetFrameSchedule(
    uint64_t controlID, const FrameScheduleUpdate& schedule);
  bool RequestRecovery(uint64_t session, uint32_t serial, bool active);
  void ProcessEvent(const NetworkEvent& event, ITransportEvents& events);
  void SetRecoveryStatus(const NetworkRecoveryStatus& status);
  bool TakeRecoveryStatus(NetworkRecoveryStatus& status);
  void SetControlStatus(const NetworkControlStatus& status);
  bool TakeControlStatus(NetworkControlStatus& status);

  InputTargetState InputState() const;
  bool WithInputAuthority(
    InputAuthorityAction action, void * opaque) const;
  InputResult ClaimInput();
  InputResult TouchInput();
  InputResult ReleaseInput(bool reset);
  InputResult SendMouseRelative(
    int32_t deltaX, int32_t deltaY, int32_t wheel, uint32_t buttons);
  InputResult SendMouseAbsolute(
    uint16_t x, uint16_t y, int32_t wheel, uint32_t buttons);
  InputResult SendKeyboard(uint8_t modifiers, const uint8_t * keys);
  InputResult ResetInput();
};
