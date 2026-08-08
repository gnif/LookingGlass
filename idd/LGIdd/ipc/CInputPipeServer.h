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

#include "CPipeEndpoint.h"
#include "InputPipeProtocol.h"
#include "input/IInputSink.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>

class CInputPipeServer : public IInputSink, private IPipeEndpointHandler
{
public:
  ~CInputPipeServer() { DeInit(); }

  bool Init();
  void DeInit();

  bool IsAvailable() const override
  {
    return (m_state.load(std::memory_order_acquire) & 1) != 0;
  }
  uint64_t GetGeneration() const override
  {
    return m_state.load(std::memory_order_acquire);
  }

  // Mouse buttons use LGInputMouseButton bits. Values outside the pipe report
  // ranges are split into multiple reports without losing motion.
  bool SendMouseRelative(
    _In_ int32_t deltaX,
    _In_ int32_t deltaY,
    _In_ int32_t wheel,
    _In_ uint32_t buttons) override;
  // Absolute coordinates are normalized to 0..32767 on each axis.
  bool SendMouseAbsolute(
    _In_range_(0, LG_INPUT_MOUSE_ABSOLUTE_MAX) uint16_t x,
    _In_range_(0, LG_INPUT_MOUSE_ABSOLUTE_MAX) uint16_t y,
    _In_ int32_t wheel,
    _In_ uint32_t buttons) override;
  // Keys are USB HID Keyboard/Keypad usage IDs; zero marks an empty slot.
  bool SendKeyboard(
    _In_ uint8_t modifiers,
    _In_reads_(LG_INPUT_KEYBOARD_KEY_COUNT) const uint8_t * keys) override;
  bool Reset() override;
  bool IsConnected() const { return m_endpoint.IsConnected(); }

private:
  bool SendMessageLocked(
    _In_ LGInputPipeMessageType type,
    _In_reads_bytes_(size) const void * payload,
    _In_ size_t size);
  bool ResetLocked();
  void Invalidate();
  void OnPipeConnected() override;
  void OnPipeDisconnected() override;
  bool OnPipeMessage(const void * message, size_t size) override;

  CPipeEndpoint         m_endpoint;
  // Odd states are available. Each endpoint transition advances the state.
  std::atomic<uint64_t> m_state { 0 };
  SRWLOCK               m_sendLock = SRWLOCK_INIT;
  uint64_t              m_sequence = 0;
  enum class MouseMode
  {
    NONE,
    RELATIVE,
    ABSOLUTE,
  };
  MouseMode             m_mouseMode = MouseMode::NONE;
  bool                  m_absoluteValid = false;
  uint16_t              m_absoluteX = 0;
  uint16_t              m_absoluteY = 0;
};

extern CInputPipeServer g_inputPipeServer;
