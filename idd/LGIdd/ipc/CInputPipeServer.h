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
#include "CPipeEndpoint.h"
#include "CSRWLock.h"
#include "InputPipeProtocol.h"
#include "input/IInputSink.h"

#include <stddef.h>
#include <stdint.h>

class CInputPipeServer : public IInputSink, private IPipeEndpointHandler
{
private:
  static constexpr size_t QUEUE_LENGTH              = 128;
  static constexpr size_t MOTION_COALESCE_THRESHOLD = QUEUE_LENGTH / 2;
  static constexpr DWORD  STATISTICS_INTERVAL_MS    = 5000;

  struct QueueItem
  {
    KVMFRInputPayload      payload;
    uint64_t               state;
    LGInputPipeMessageType type;
    bool                   pureMotion;
  };

  enum class MouseMode
  {
    NONE,
    RELATIVE_INPUT,
    ABSOLUTE_INPUT,
  };

  CPipeEndpoint m_endpoint;

  // Odd states are available. Endpoint changes and resyncs advance the state.
  std::atomic<uint64_t> m_state { 0 };

  CSRWLock m_queueLock;
  CSRWLock m_connectionLock;
  HANDLE   m_stopEvent  = nullptr;
  HANDLE   m_queueEvent = nullptr;
  HANDLE   m_thread     = nullptr;

  QueueItem m_queue[QUEUE_LENGTH] = {};
  size_t    m_queueHead            = 0;
  size_t    m_queueCount           = 0;
  uint64_t  m_sequence             = 0;
  uint64_t  m_feedbackSequence     = 0;

  std::atomic<uint8_t> m_keyboardLEDs      { 0 };
  std::atomic<bool>    m_keyboardLEDsValid { false };

  MouseMode m_mouseMode       = MouseMode::NONE;
  uint16_t  m_absoluteX       = 0;
  uint16_t  m_absoluteY       = 0;
  uint32_t  m_relativeButtons = 0;
  uint32_t  m_absoluteButtons = 0;

  uint64_t m_statEnqueued          = 0;
  uint64_t m_statRelativeCoalesced = 0;
  uint64_t m_statAbsoluteCoalesced = 0;
  uint64_t m_statResyncs           = 0;
  uint64_t m_statResyncDiscarded   = 0;
  size_t   m_statQueueHighWater    = 0;

  uint64_t      m_statSent            = 0;
  uint64_t      m_statStale           = 0;
  uint64_t      m_statWriteFailed     = 0;
  uint64_t      m_statSlowWrites      = 0;
  int64_t       m_statWriteTicks      = 0;
  int64_t       m_statMaxWriteTicks   = 0;
  ULONGLONG     m_lastStatistics      = 0;
  LARGE_INTEGER m_performanceFrequency = {};

  bool QueueLocked(LGInputPipeMessageType type,
    const KVMFRInputPayload& payload, bool pureMotion);
  bool QueueRawLocked(LGInputPipeMessageType type,
    const KVMFRInputPayload& payload, bool pureMotion);
  bool QueueResetLocked();
  void ResyncLocked();
  bool Pop(QueueItem& item);
  bool Send(const QueueItem& item);
  void Invalidate(uint64_t state, bool requireMatch);
  void LogStatistics();
  void Thread();

  static DWORD WINAPI ThreadProc(void * context);

  void OnPipeConnected() override;
  void OnPipeDisconnected() override;
  bool OnPipeMessage(const void * message, size_t size) override;

public:
  ~CInputPipeServer() { DeInit(); }

  bool Init();
  void DeInit();

  uint64_t GetState() const override
  {
    return Atomic::Load(m_state, std::memory_order_acquire);
  }

  bool GetKeyboardLEDs(uint8_t& leds) const override
  {
    if (!Atomic::Load(m_keyboardLEDsValid, std::memory_order_acquire))
      return false;
    leds = Atomic::Load(m_keyboardLEDs, std::memory_order_acquire);
    return true;
  }

  bool SendMouseRelative(
    _In_ int32_t deltaX,
    _In_ int32_t deltaY,
    _In_ int32_t wheel,
    _In_ uint32_t buttons) override;
  bool SendMouseAbsolute(
    _In_range_(0, LG_INPUT_MOUSE_ABSOLUTE_MAX) uint16_t x,
    _In_range_(0, LG_INPUT_MOUSE_ABSOLUTE_MAX) uint16_t y,
    _In_ int32_t wheel,
    _In_ uint32_t buttons) override;
  bool SendKeyboard(
    _In_ uint8_t modifiers,
    _In_reads_(LG_INPUT_KEYBOARD_KEY_COUNT) const uint8_t * keys) override;
  bool Reset() override;
};

extern CInputPipeServer g_inputPipeServer;
