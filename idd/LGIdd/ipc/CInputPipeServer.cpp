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
#include "CSRWLock.h"
#include "InputPipeProtocol.h"

#include <string.h>

CInputPipeServer g_inputPipeServer;

static constexpr int32_t MAX_SPLIT_REPORTS = 4;
static constexpr int32_t MAX_MOUSE_DELTA =
  INT16_MAX * MAX_SPLIT_REPORTS;
static constexpr int32_t MIN_MOUSE_DELTA =
  INT16_MIN * MAX_SPLIT_REPORTS;
static constexpr int32_t MAX_MOUSE_WHEEL =
  INT8_MAX * MAX_SPLIT_REPORTS;
static constexpr int32_t MIN_MOUSE_WHEEL =
  LG_INPUT_MOUSE_WHEEL_MIN * MAX_SPLIT_REPORTS;

bool CInputPipeServer::Init()
{
  m_state.store(0, std::memory_order_release);
  {
    CSRWExclusiveLock lock(&m_sendLock);
    m_sequence      = 0;
    m_mouseMode     = MouseMode::NONE;
    m_absoluteValid = false;
    m_absoluteX     = 0;
    m_absoluteY     = 0;
  }

  m_endpoint.SetHandler(this);
  return m_endpoint.Start(
    LG_INPUT_PIPE_NAME,
    CPipeEndpoint::Mode::Server,
    sizeof(LGInputPipeMessage));
}

void CInputPipeServer::DeInit()
{
  Invalidate();
  m_endpoint.Stop();
}

bool CInputPipeServer::SendMouseRelative(
  int32_t deltaX,
  int32_t deltaY,
  int32_t wheel,
  uint32_t buttons)
{
  if (deltaX < MIN_MOUSE_DELTA || deltaX > MAX_MOUSE_DELTA ||
      deltaY < MIN_MOUSE_DELTA || deltaY > MAX_MOUSE_DELTA ||
      wheel < MIN_MOUSE_WHEEL || wheel > MAX_MOUSE_WHEEL ||
      (buttons & ~static_cast<uint32_t>(LG_INPUT_MOUSE_BUTTON_MASK)))
    return false;

  CSRWExclusiveLock lock(&m_sendLock);
  if (!IsAvailable())
    return false;

  if (m_mouseMode == MouseMode::ABSOLUTE)
  {
    const LGInputPipeMouseAbsolute neutral = {
      0,
      m_absoluteX,
      m_absoluteY,
      0,
    };
    if (!SendMessageLocked(
        LG_INPUT_PIPE_MESSAGE_MOUSE_ABSOLUTE,
        &neutral,
        sizeof(neutral)))
    {
      Invalidate();
      return false;
    }
    m_mouseMode = MouseMode::NONE;
  }

  do
  {
    const int16_t x = deltaX > INT16_MAX ? INT16_MAX :
      deltaX < INT16_MIN ? INT16_MIN : static_cast<int16_t>(deltaX);
    const int16_t y = deltaY > INT16_MAX ? INT16_MAX :
      deltaY < INT16_MIN ? INT16_MIN : static_cast<int16_t>(deltaY);
    const int8_t wheelDelta = wheel > INT8_MAX ? INT8_MAX :
      wheel < LG_INPUT_MOUSE_WHEEL_MIN ? LG_INPUT_MOUSE_WHEEL_MIN :
      static_cast<int8_t>(wheel);

    const LGInputPipeMouseRelative payload = {
      static_cast<uint8_t>(buttons),
      x,
      y,
      wheelDelta,
    };
    if (!SendMessageLocked(
        LG_INPUT_PIPE_MESSAGE_MOUSE_RELATIVE, &payload, sizeof(payload)))
    {
      Invalidate();
      return false;
    }

    m_mouseMode = MouseMode::RELATIVE;
    deltaX -= x;
    deltaY -= y;
    wheel  -= wheelDelta;
  }
  while (deltaX || deltaY || wheel);

  return true;
}

bool CInputPipeServer::SendMouseAbsolute(
  uint16_t x,
  uint16_t y,
  int32_t wheel,
  uint32_t buttons)
{
  if (x > LG_INPUT_MOUSE_ABSOLUTE_MAX ||
      y > LG_INPUT_MOUSE_ABSOLUTE_MAX ||
      wheel < MIN_MOUSE_WHEEL || wheel > MAX_MOUSE_WHEEL ||
      (buttons & ~static_cast<uint32_t>(LG_INPUT_MOUSE_BUTTON_MASK)))
    return false;

  CSRWExclusiveLock lock(&m_sendLock);
  if (!IsAvailable())
    return false;

  if (m_mouseMode == MouseMode::RELATIVE)
  {
    const LGInputPipeMouseRelative neutral = {};
    if (!SendMessageLocked(
        LG_INPUT_PIPE_MESSAGE_MOUSE_RELATIVE,
        &neutral,
        sizeof(neutral)))
    {
      Invalidate();
      return false;
    }
    m_mouseMode = MouseMode::NONE;
  }

  do
  {
    const int8_t wheelDelta = wheel > INT8_MAX ? INT8_MAX :
      wheel < LG_INPUT_MOUSE_WHEEL_MIN ? LG_INPUT_MOUSE_WHEEL_MIN :
      static_cast<int8_t>(wheel);
    const LGInputPipeMouseAbsolute payload = {
      static_cast<uint8_t>(buttons),
      x,
      y,
      wheelDelta,
    };
    if (!SendMessageLocked(
        LG_INPUT_PIPE_MESSAGE_MOUSE_ABSOLUTE, &payload, sizeof(payload)))
    {
      Invalidate();
      return false;
    }
    m_mouseMode = MouseMode::ABSOLUTE;
    m_absoluteValid = true;
    m_absoluteX = x;
    m_absoluteY = y;
    wheel -= wheelDelta;
  }
  while (wheel);

  return true;
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

  CSRWExclusiveLock lock(&m_sendLock);
  const bool sent = IsAvailable() &&
    SendMessageLocked(
      LG_INPUT_PIPE_MESSAGE_KEYBOARD,
      &payload,
      sizeof(payload));
  if (!sent)
    Invalidate();
  return sent;
}

bool CInputPipeServer::Reset()
{
  CSRWExclusiveLock lock(&m_sendLock);
  const bool reset = IsAvailable() &&
    ResetLocked();
  if (!reset)
    Invalidate();
  return reset;
}

bool CInputPipeServer::ResetLocked()
{
  const LGInputPipeMouseRelative relative = {};
  const LGInputPipeMouseAbsolute absolute = {
    0,
    m_absoluteX,
    m_absoluteY,
    0,
  };
  const LGInputPipeKeyboard keyboard = {};

  if (!SendMessageLocked(
      LG_INPUT_PIPE_MESSAGE_MOUSE_RELATIVE,
      &relative,
      sizeof(relative)) ||
      (m_absoluteValid &&
       !SendMessageLocked(
         LG_INPUT_PIPE_MESSAGE_MOUSE_ABSOLUTE,
         &absolute,
         sizeof(absolute))) ||
      !SendMessageLocked(
        LG_INPUT_PIPE_MESSAGE_KEYBOARD,
        &keyboard,
        sizeof(keyboard)))
    return false;

  m_mouseMode = MouseMode::NONE;
  return true;
}

bool CInputPipeServer::SendMessageLocked(
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

  message.sequence = ++m_sequence;
  const bool sent = m_endpoint.Send(&message, sizeof(message));
  return sent;
}

void CInputPipeServer::Invalidate()
{
  uint64_t state = m_state.load(std::memory_order_acquire);
  while ((state & 1) && !m_state.compare_exchange_weak(
      state, state + 1, std::memory_order_acq_rel))
  {
  }
}

void CInputPipeServer::OnPipeConnected()
{
  Invalidate();

  CSRWExclusiveLock lock(&m_sendLock);
  const bool ready = ResetLocked();
  if (!ready)
  {
    DEBUG_WARN("Failed to neutralize the LGInput endpoint");
    return;
  }

  m_state.fetch_add(1, std::memory_order_acq_rel);
}

void CInputPipeServer::OnPipeDisconnected()
{
  Invalidate();
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
