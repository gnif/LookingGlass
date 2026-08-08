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

#include <stddef.h>
#include <stdint.h>

class CInputPipeServer : private IPipeEndpointHandler
{
public:
  ~CInputPipeServer() { DeInit(); }

  bool Init();
  void DeInit();

  // Mouse buttons use LGInputMouseButton bits. Wheel values are -127..127.
  bool SendMouseRelative(
    _In_ int16_t deltaX,
    _In_ int16_t deltaY,
    _In_ int8_t wheel,
    _In_ uint8_t buttons);
  // Absolute coordinates are normalized to 0..32767 on each axis.
  bool SendMouseAbsolute(
    _In_range_(0, LG_INPUT_MOUSE_ABSOLUTE_MAX) uint16_t x,
    _In_range_(0, LG_INPUT_MOUSE_ABSOLUTE_MAX) uint16_t y,
    _In_ int8_t wheel,
    _In_ uint8_t buttons);
  // Keys are USB HID Keyboard/Keypad usage IDs; zero marks an empty slot.
  bool SendKeyboard(
    _In_ uint8_t modifiers,
    _In_reads_(LG_INPUT_KEYBOARD_KEY_COUNT) const uint8_t * keys);
  bool IsConnected() const { return m_endpoint.IsConnected(); }

private:
  bool SendMessage(
    _In_ LGInputPipeMessageType type,
    _In_reads_bytes_(size) const void * payload,
    _In_ size_t size);
  bool OnPipeMessage(const void * message, size_t size) override;

  CPipeEndpoint m_endpoint;
  SRWLOCK m_sendLock = SRWLOCK_INIT;
  uint64_t m_sequence = 0;
};

extern CInputPipeServer g_inputPipeServer;
