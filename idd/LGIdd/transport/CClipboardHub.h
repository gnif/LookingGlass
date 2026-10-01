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

class CClipboardHub final : private IClipboardChannelHandler
{
private:
  static constexpr unsigned MAX_SOURCES = TRANSPORT_MAX_INSTANCES;

  struct Source final : public IClipboardTarget
  {
    CClipboardHub    * owner          = nullptr;
    BackendId          backend        = 0;
    uint32_t           epoch          = 0;
    uint32_t           generation     = 0;
    IClipboardSource * endpoint       = nullptr;
    bool               running        = false;
    bool               active         = false;
    bool               reserved       = false;
    bool               failed         = false;
    bool               failurePending = false;

    ClipboardClaimResult Claim(const ClipboardSourceId& source,
      KVMFRClipboardMessage& cached) override;
    void Release(const ClipboardSourceId& source) override;
    ClipboardChannelResult SendClipboard(
      const KVMFRClipboardMessage& record,
      const uint8_t * data) override;
    ClipboardChannelResult SendClipboardBatch(
      const ClipboardChannelWrite * records,
      size_t count, size_t& accepted) override;
    void ClipboardReceiveReady() override;
    void ClipboardFailed() override;
  };

  CClipboardChannel&  m_channel;
  CSRWLock            m_lifecycleLock;
  CSRWLock            m_callbackLock;
  CSRWLock            m_lock;

  Source                m_sources[MAX_SOURCES];
  Source              * m_owner           = nullptr;
  Source              * m_cachedSource    = nullptr;
  ClipboardSourceId     m_ownerId;
  KVMFRClipboardMessage m_cachedClipboard = {};
  uint64_t              m_channelEpoch    = 0;
  uint32_t              m_generation      = 1;
  bool                  m_cachedValid     = false;
  bool                  m_available       = false;
  bool                  m_resetting       = false;
  bool                  m_stopped         = false;

  uint32_t NextGenerationNL();
  bool BindingPresentNL(const Source& source) const;
  bool BindingValidNL(const Source& source) const;
  bool OwnerIsNL(
    const Source& source, const ClipboardSourceId& id) const;
  bool MarkFailed(Source& source);
  void CachePublicationNL(
    const KVMFRClipboardMessage& record, Source * source);
  void InvalidateSourceCacheNL(const Source& source);
  ClipboardChannelResult HoldOrDiscard(
    const KVMFRClipboardMessage& record);

  ClipboardClaimResult Claim(Source& source,
    const ClipboardSourceId& id, KVMFRClipboardMessage& cached);
  void Release(Source& source, const ClipboardSourceId& id);
  ClipboardChannelResult SendClipboard(Source& source,
    const KVMFRClipboardMessage& record, const uint8_t * data);
  ClipboardChannelResult SendClipboardBatch(Source& source,
    const ClipboardChannelWrite * records,
    size_t count, size_t& accepted);
  void ClipboardReceiveReady(Source& source);
  void ClipboardFailed(Source& source);

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
};
