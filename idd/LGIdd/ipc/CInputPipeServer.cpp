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

#include "ipc/CInputPipeServer.h"

#include "CDebug.h"
#include "InputPipeProtocol.h"

#include <string.h>

CInputPipeServer g_inputPipeServer;

bool CInputPipeServer::Init()
{
  AcquireSRWLockExclusive(&m_sendLock);
  m_sequence = 0;
  ReleaseSRWLockExclusive(&m_sendLock);

  m_endpoint.SetHandler(this);
  return m_endpoint.Start(
    LG_INPUT_PIPE_NAME,
    CPipeEndpoint::Mode::Server,
    sizeof(LGInputPipeMessage));
}

void CInputPipeServer::DeInit()
{
  m_endpoint.Stop();
}

bool CInputPipeServer::SendMouseRelative(
  int16_t deltaX,
  int16_t deltaY,
  int8_t wheel,
  uint8_t buttons)
{
  if (wheel < LG_INPUT_MOUSE_WHEEL_MIN ||
      (buttons & ~LG_INPUT_MOUSE_BUTTON_MASK))
    return false;

  const LGInputPipeMouseRelative payload = {
    buttons,
    deltaX,
    deltaY,
    wheel,
  };
  return SendMessage(
    LG_INPUT_PIPE_MESSAGE_MOUSE_RELATIVE,
    &payload,
    sizeof(payload));
}

bool CInputPipeServer::SendMouseAbsolute(
  uint16_t x,
  uint16_t y,
  int8_t wheel,
  uint8_t buttons)
{
  if (x > LG_INPUT_MOUSE_ABSOLUTE_MAX ||
      y > LG_INPUT_MOUSE_ABSOLUTE_MAX ||
      wheel < LG_INPUT_MOUSE_WHEEL_MIN ||
      (buttons & ~LG_INPUT_MOUSE_BUTTON_MASK))
    return false;

  const LGInputPipeMouseAbsolute payload = {
    buttons,
    x,
    y,
    wheel,
  };
  return SendMessage(
    LG_INPUT_PIPE_MESSAGE_MOUSE_ABSOLUTE,
    &payload,
    sizeof(payload));
}

bool CInputPipeServer::SendKeyboard(
  uint8_t modifiers,
  const uint8_t * keys)
{
  if (!keys)
    return false;

  LGInputPipeKeyboard payload = {};
  payload.modifiers = modifiers;
  for (size_t i = 0; i < LG_INPUT_KEYBOARD_KEY_COUNT; ++i)
  {
    if (keys[i] > LG_INPUT_KEYBOARD_USAGE_MAX)
      return false;
    payload.keys[i] = keys[i];
  }

  return SendMessage(
    LG_INPUT_PIPE_MESSAGE_KEYBOARD,
    &payload,
    sizeof(payload));
}

bool CInputPipeServer::SendMessage(
  LGInputPipeMessageType type,
  const void * payload,
  size_t size)
{
  if (!payload || !size || size > LG_INPUT_PIPE_MAX_PAYLOAD_SIZE)
    return false;

  LGInputPipeMessage message = {};
  message.magic = LG_INPUT_PIPE_MAGIC;
  message.version = LG_INPUT_PIPE_VERSION;
  message.type = type;
  message.payloadSize = static_cast<uint32_t>(size);
  memcpy(message.payload, payload, size);

  AcquireSRWLockExclusive(&m_sendLock);
  message.sequence = ++m_sequence;
  const bool sent = m_endpoint.Send(&message, sizeof(message));
  ReleaseSRWLockExclusive(&m_sendLock);
  return sent;
}

bool CInputPipeServer::OnPipeMessage(
  const void * message,
  size_t size)
{
  UNREFERENCED_PARAMETER(message);
  UNREFERENCED_PARAMETER(size);
  DEBUG_WARN("LGInput sent an unexpected message");
  return false;
}
