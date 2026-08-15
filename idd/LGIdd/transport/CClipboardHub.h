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

#include "CClipboardChannel.h"
#include "CSRWLock.h"
#include "transport/IClipboardSource.h"
#include "transport/ITransport.h"

class CClipboardHub final : public IClipboardTarget,
  private IClipboardChannelHandler
{
private:
  CClipboardChannel& m_channel;
  CSRWLock            m_lifecycleLock;
  CSRWLock            m_callbackLock;
  CSRWLock            m_lock;

  IClipboardSource * m_source          = nullptr;
  BackendId          m_backend         = 0;
  uint32_t           m_epoch           = 0;
  uint64_t           m_channelEpoch    = 0;
  uint32_t           m_generation      = 1;
  bool               m_available       = false;
  bool               m_active          = false;
  bool               m_running         = false;
  bool               m_reserved        = false;
  bool               m_failed          = false;
  bool               m_failurePending  = false;
  bool               m_stopped         = false;

  void AdvanceGenerationNL();
  bool MarkFailed(IClipboardSource& source);
  ClipboardChannelResult HoldOrDiscard(
    const KVMFRClipboardMessage& record);

  void ClipboardState(bool available, uint64_t epoch) override;
  ClipboardChannelResult ClipboardRecord(
    const KVMFRClipboardMessage& record,
    std::vector<uint8_t>&& data) override;
  void ClipboardReset(uint64_t epoch, uint32_t reason) override;

public:
  explicit CClipboardHub(CClipboardChannel& channel);
  ~CClipboardHub() override;

  CClipboardHub(const CClipboardHub&) = delete;
  CClipboardHub& operator=(const CClipboardHub&) = delete;

  bool Bind(BackendId backend, uint32_t epoch, IClipboardSource& source);
  void Unbind(BackendId backend, uint32_t epoch);
  bool TakeFailure(SourceKey& source);
  void Stop();

  ClipboardChannelResult SendClipboard(
    const KVMFRClipboardMessage& record,
    const uint8_t * data) override;
  void ClipboardReceiveReady() override;
  void ClipboardFailed() override;
};
