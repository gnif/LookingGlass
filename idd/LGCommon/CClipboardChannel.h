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

#include "Atomic.h"
#include "ClipboardRing.h"
#include "CSRWLock.h"

#include <Windows.h>

#include <atomic>
#include <stdint.h>

enum class ClipboardChannelResult
{
  ACCEPTED,
  BUSY,
  FAILED,
};

class IClipboardChannelHandler
{
public:
  virtual ~IClipboardChannelHandler() = default;

  virtual void ClipboardState(bool available, uint64_t epoch) = 0;
  // data is borrowed and remains valid only for the duration of this call.
  // BUSY leaves the shared ring slot occupied so it can be retried later.
  virtual ClipboardChannelResult ClipboardRecord(
    const KVMFRClipboardMessage& record, const uint8_t * data) = 0;
  virtual void ClipboardReset(uint64_t epoch, uint32_t reason) = 0;
};

class IClipboardChannelDoorbell
{
public:
  virtual ~IClipboardChannelDoorbell() = default;

  virtual bool ClipboardKick(uint64_t epoch) = 0;
  virtual void ClipboardResetPeer(uint64_t epoch, uint32_t reason) = 0;
};

class CClipboardChannel
{
private:
  static constexpr DWORD POLL_MS = 50;

  enum class DrainResult
  {
    IDLE,
    BUSY,
    STOPPED,
    CORRUPT,
  };

  CSRWLock m_lifecycleLock;
  CSRWLock m_handlerLock;
  CSRWLock m_writeLock;

  HANDLE             m_mapping = nullptr;
  ClipboardMapping * m_view    = nullptr;
  ClipboardRing    * m_in      = nullptr;
  ClipboardRing    * m_out     = nullptr;
  HANDLE             m_stop    = nullptr;
  HANDLE             m_kick    = nullptr;
  HANDLE             m_thread  = nullptr;
  DWORD              m_threadId = 0;
  uint64_t           m_epoch   = 0;
  uint64_t           m_instance = 0;
  bool               m_deferredDetach = false;

  IClipboardChannelHandler  * m_handler  = nullptr;
  IClipboardChannelDoorbell * m_doorbell = nullptr;
  std::atomic<bool>           m_available { false };

  DrainResult Drain();
  void Thread();
  void Fail(uint64_t instance, uint32_t reason, bool resetPeer = true);
  void CleanupDeferredDetach();
  void PublishState(bool available, uint64_t epoch);
  ClipboardChannelResult PublishRecord(
    const KVMFRClipboardMessage& record, const uint8_t * data);
  void PublishReset(uint64_t epoch, uint32_t reason);

  static DWORD WINAPI ThreadProc(void * context);

public:
  CClipboardChannel() = default;
  ~CClipboardChannel();

  CClipboardChannel(const CClipboardChannel&) = delete;
  CClipboardChannel& operator=(const CClipboardChannel&) = delete;

  bool Attach(HANDLE mapping, uint64_t epoch, bool helper,
    IClipboardChannelDoorbell& doorbell);
  void Detach();
  void Kick(uint64_t epoch);
  void Reset(uint64_t epoch, uint32_t reason);

  ClipboardChannelResult Send(
    const KVMFRClipboardMessage& record, const void * data = nullptr);

  void SetHandler(IClipboardChannelHandler * handler);
  void ClearHandler(IClipboardChannelHandler * handler);

  bool Available() const
  {
    return Atomic::Load(m_available, std::memory_order_acquire);
  }
  uint64_t Epoch();
};
