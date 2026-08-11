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

#include <stdint.h>

struct KVMFRR;
struct KVMFRRRequest;
struct KVMFRRStatus;

class CIVSHMEM;

class CRecovery
{
public:
  struct Request
  {
    uint64_t session = 0;
    uint32_t serial  = 0;
    bool     valid   = false;
    bool     active  = false;
  };

private:
  CSRWLock m_lock;
  KVMFRR * m_data = nullptr;

  uint64_t m_session       = 0;
  uint64_t m_nextHeartbeat = 0;
  uint64_t m_deadline      = 0;
  uint32_t m_lastSerial    = 0;
  uint32_t m_statusSerial  = 0;
  uint32_t m_request       = 0;
  bool     m_syncReady     = false;
  bool     m_replay        = false;
  bool     m_waiting       = false;

  static bool ReadRequest(KVMFRRRequest& source, KVMFRRRequest& result);
  static bool ReadStatus(KVMFRRStatus& source, KVMFRRStatus& result);
  static bool SerialNewer(uint32_t serial, uint32_t reference);
  bool OwnsSession();
  uint32_t NextTicket();
  void Publish(uint32_t serial, uint32_t request,
    uint32_t state, uint32_t error);

public:
  bool Initialize(CIVSHMEM& ivshmem);
  void Sync();
  Request Process();
  void SetStatus(uint64_t session, uint32_t serial, bool active,
    uint32_t state, uint32_t error);
};
