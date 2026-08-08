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

#include "transport/IInputTransport.h"
#include "common/LGMPConfig.h"

#include <Windows.h>

#include <stdint.h>

extern "C" {
  #include "lgmp/host.h"
}

class CLGMPHost;
class IInputSink;
struct KVMFRInputMessage;

class CLGMPInputTransport final : public IInputTransport
{
private:
  static constexpr ULONGLONG OWNER_LEASE_MS = 500;
  static constexpr ULONGLONG ACTIVE_POLL_MS  = 50;

  CLGMPHost& m_host;

  PLGMPHostQueue m_queue                          = nullptr;
  PLGMPMemory    m_statusMemory[LGMP_Q_INPUT_LEN] = {};
  IInputSink   * m_sink                           = nullptr;

  SRWLOCK m_lifecycleLock = SRWLOCK_INIT;
  HANDLE  m_stopEvent     = nullptr;
  HANDLE  m_pollTimer     = nullptr;
  HANDLE  m_thread        = nullptr;

  uint32_t  m_ownerClientID       = 0;
  uint32_t  m_ownerGeneration     = 0;
  uint32_t  m_ownerSequence       = 0;
  ULONGLONG m_ownerDeadline       = 0;
  uint64_t  m_sinkState           = 0;
  uint32_t  m_endpointGeneration = 0;
  uint32_t  m_statusSerial        = 0;
  bool      m_statusDirty         = false;

  bool Initialize();
  void DeInit();
  void UpdateSinkState(uint64_t state);
  void PublishStatus();
  bool DrainMessages();
  bool ProcessMessage(uint32_t sourceClientID,
    const KVMFRInputMessage& message);
  bool ValidatePayload(const KVMFRInputMessage& message) const;
  bool IsOwner(uint32_t sourceClientID, uint32_t generation) const;
  bool Claim(uint32_t sourceClientID,
    const KVMFRInputMessage& message);
  void RenewLease();
  void ReleaseOwner(bool reset, const char * reason);
  void CheckOwner();
  void Thread();

  static DWORD CALLBACK ThreadProc(void * context);

  friend class CLGMPTransport;

public:
  explicit CLGMPInputTransport(CLGMPHost& host) :
    m_host(host) {}
  ~CLGMPInputTransport() override;

  CLGMPInputTransport(const CLGMPInputTransport&) = delete;
  CLGMPInputTransport& operator=(const CLGMPInputTransport&) = delete;

  bool Start(IInputSink& sink) override;
  void Stop() override;
};
