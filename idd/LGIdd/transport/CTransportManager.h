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

#include "transport/CControlHub.h"
#include "transport/CFrameHub.h"
#include "transport/ITransport.h"

#include <memory>
#include <vector>

class CTransportManager final
{
public:
  using CreateFn = std::unique_ptr<ITransport> (*)();
  using OpenResult = ITransport::OpenResult;
  using ProcessResult = ITransport::ProcessResult;
  using Recovery = ITransport::Recovery;

private:
  enum class State
  {
    CLOSED,
    OPEN,
    INITIALIZED,
    READY,
    RETRY,
    FAILED,
    STOPPED,
  };

  struct Entry
  {
    BackendId                   id;
    const char                * name;
    bool                        required;
    bool                        primary;
    CreateFn                    create;
    std::unique_ptr<ITransport> transport;
    State                       state   = State::CLOSED;
    uint32_t                    epoch   = 1;
    uint64_t                    retryAt = 0;
    bool                        controlAdded = false;
    bool                        frameAdded   = false;
  };

  std::vector<std::unique_ptr<Entry>> m_entries;
  CControlHub                         m_control;
  CFrameHub                           m_frames;
  Entry                              * m_primary     = nullptr;
  bool                                 m_initialized = false;
  bool                                 m_setup       = false;
  bool                                 m_exposed     = false;
  size_t                               m_alignment   = 0;

  OpenResult OpenEntry(Entry& entry);
  bool InitializeEntry(Entry& entry);
  bool SetupEntry(Entry& entry);
  void RetryEntry(Entry& entry, uint64_t now);
  void HandleProcessResult(Entry& entry, ProcessResult result);
  void ScheduleRetry(Entry& entry);
  void RemoveServices(Entry& entry);
  ITransport& Primary();

public:
  CTransportManager() = default;
  ~CTransportManager();

  CTransportManager(const CTransportManager&) = delete;
  CTransportManager& operator=(const CTransportManager&) = delete;

  bool Add(BackendId id, const char * name, bool required, bool primary,
    CreateFn create);

  OpenResult Open();
  bool Initialize();
  bool Setup(size_t alignment);
  ProcessResult Process(ITransportEvents& events);
  void Stop();
  void SyncRecovery();
  void RecoveryStatus(uint64_t session, uint32_t serial, bool active,
    Recovery state, uint32_t error);

  FrameMemoryLimits GetMemoryLimits() const;
  DirectFrameBufferMemory GetDirectMemory() const;

  IFrameTransport& Frames();
  IControlTransport& Control();
  IInputTransport * Input();
};
