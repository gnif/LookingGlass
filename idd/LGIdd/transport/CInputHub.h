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
#include "transport/IInputSource.h"
#include "transport/IInputTransport.h"
#include "transport/ITransport.h"

#include <Windows.h>

class IInputSink;

class CInputHub final : public IInputTransport
{
public:
  struct InteractionPermit
  {
    uint64_t serial = 0;
    SourceKey source;
  };

private:
  static constexpr unsigned MAX_SOURCES = 8;
  static constexpr uint64_t OWNER_LEASE_MS = 500;
  static constexpr uint64_t INTERACTION_LEASE_MS = 500;

  struct Source final : public IInputTarget
  {
    CInputHub   * owner   = nullptr;
    BackendId     backend = 0;
    uint32_t      epoch   = 0;
    IInputSource * endpoint = nullptr;
    bool           running = false;
    bool           active = false;
    bool           reserved = false;
    bool           failed = false;
    bool           failurePending = false;

    SourceKey Key(const InputSourceId& source) const;
    InputTargetState GetState(const InputSourceId& source) override;
    void Failed() override;
    InputResult Claim(const InputSourceId& source) override;
    InputResult Touch(const InputSourceId& source) override;
    InputResult Release(
      const InputSourceId& source, bool reset) override;
    InputResult SendMouseRelative(const InputSourceId& source,
      int32_t deltaX, int32_t deltaY, int32_t wheel,
      uint32_t buttons) override;
    InputResult SendMouseAbsolute(const InputSourceId& source,
      uint16_t x, uint16_t y, int32_t wheel,
      uint32_t buttons) override;
    InputResult SendKeyboard(const InputSourceId& source,
      uint8_t modifiers, const uint8_t * keys) override;
    InputResult Reset(const InputSourceId& source) override;
  };

  mutable CSRWLock m_lock;
  CSRWLock         m_lifecycleLock;
  Source           m_sources[MAX_SOURCES];
  IInputSink     * m_sink = nullptr;
  uint64_t         m_sinkState = 0;
  SourceKey        m_owner;
  uint64_t         m_ownerDeadline = 0;
  SourceKey        m_interactionOwner;
  uint64_t         m_interactionDeadline = 0;
  uint64_t         m_interactionSerial = 1;
  bool             m_started = false;

  bool SourceValid(const SourceKey& source) const;
  bool BindingPresent(const SourceKey& source) const;
  bool BindingValid(const SourceKey& source) const;
  bool OwnerValid(const SourceKey& source) const;
  void ClearInteraction();
  void InvalidateInteraction();
  void AdvanceInteractionSerial();
  bool CheckState();
  InputTargetState GetState(const SourceKey& source);
  void Failed(Source& source);
  InputResult Claim(const SourceKey& source);
  InputResult Touch(const SourceKey& source);
  InputResult Release(const SourceKey& source, bool reset);
  InputResult SendMouseRelative(const SourceKey& source,
    int32_t deltaX, int32_t deltaY, int32_t wheel, uint32_t buttons);
  InputResult SendMouseAbsolute(const SourceKey& source,
    uint16_t x, uint16_t y, int32_t wheel, uint32_t buttons);
  InputResult SendKeyboard(const SourceKey& source,
    uint8_t modifiers, const uint8_t * keys);
  InputResult Reset(const SourceKey& source);

public:
  CInputHub();
  ~CInputHub() override;

  CInputHub(const CInputHub&) = delete;
  CInputHub& operator=(const CInputHub&) = delete;

  bool Bind(BackendId backend, uint32_t epoch, IInputSource& source);
  void Unbind(BackendId backend, uint32_t epoch);
  InteractionResult CheckInteraction(
    SourceKey& source, InteractionPermit& permit);
  void CommitInteraction(
    const SourceKey& source, const InteractionPermit& permit);
  void RevokeInteraction(BackendId backend, uint32_t epoch);
  bool TakeFailure(SourceKey& source);

  bool Start(IInputSink& sink) override;
  void Stop() override;
};
