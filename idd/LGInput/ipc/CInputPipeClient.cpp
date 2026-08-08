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

#include "CInputPipeClient.h"

#include "CDebug.h"
#include "InputPipeProtocol.h"
#include "../CHIDDevice.h"
#include "../HIDReports.h"

#include <string.h>

bool CInputPipeClient::Start()
{
  m_lastSequence = 0;
  m_endpoint.SetHandler(this);
  return m_endpoint.Start(
    LG_INPUT_PIPE_NAME,
    CPipeEndpoint::Mode::Client,
    sizeof(LGInputPipeMessage));
}

void CInputPipeClient::Stop()
{
  const bool wasRunning = m_endpoint.IsRunning();
  m_endpoint.Stop();
  m_lastSequence = 0;
  if (wasRunning)
    CHIDDevice::ClearReports();
}

void CInputPipeClient::OnPipeConnected()
{
  m_lastSequence = 0;
  DEBUG_INFO("Connected to the LGIdd input transport");
}

void CInputPipeClient::OnPipeDisconnected()
{
  m_lastSequence = 0;
  CHIDDevice::ClearReports();
  DEBUG_INFO("Disconnected from the LGIdd input transport; reconnecting");
}

bool CInputPipeClient::OnPipeMessage(
  const void * frame,
  size_t size)
{
  if (size != sizeof(LGInputPipeMessage))
    return false;

  const LGInputPipeMessage & message =
    *static_cast<const LGInputPipeMessage *>(frame);
  if (message.magic != LG_INPUT_PIPE_MAGIC ||
      message.version != LG_INPUT_PIPE_VERSION ||
      !message.payloadSize ||
      message.payloadSize > sizeof(message.payload))
    return false;

  if (!message.sequence ||
      (m_lastSequence && message.sequence != m_lastSequence + 1))
  {
    DEBUG_WARN("LGInput pipe report sequence changed unexpectedly");
    return false;
  }
  bool handled = false;
  switch (message.type)
  {
    case LG_INPUT_PIPE_MESSAGE_MOUSE_RELATIVE:
      handled = HandleMouseRelative(
        message.payload, message.payloadSize);
      break;

    case LG_INPUT_PIPE_MESSAGE_MOUSE_ABSOLUTE:
      handled = HandleMouseAbsolute(
        message.payload, message.payloadSize);
      break;

    case LG_INPUT_PIPE_MESSAGE_KEYBOARD:
      handled = HandleKeyboard(message.payload, message.payloadSize);
      break;

    default:
      return false;
  }

  if (!handled)
    return false;

  m_lastSequence = message.sequence;
  return true;
}

bool CInputPipeClient::HandleMouseRelative(
  const void * payload,
  size_t size)
{
  if (size != sizeof(LGInputPipeMouseRelative))
    return false;

  LGInputPipeMouseRelative input = {};
  memcpy(&input, payload, sizeof(input));
  if (input.wheel < LG_INPUT_MOUSE_WHEEL_MIN ||
      (input.buttons & ~LG_INPUT_MOUSE_BUTTON_MASK))
    return false;

  const HIDMouseRelativeReport report = {
    HID_REPORT_ID_MOUSE_RELATIVE,
    input.buttons,
    input.deltaX,
    input.deltaY,
    input.wheel,
  };
  return SubmitReport(&report, sizeof(report));
}

bool CInputPipeClient::HandleMouseAbsolute(
  const void * payload,
  size_t size)
{
  if (size != sizeof(LGInputPipeMouseAbsolute))
    return false;

  LGInputPipeMouseAbsolute input = {};
  memcpy(&input, payload, sizeof(input));
  if (input.x > LG_INPUT_MOUSE_ABSOLUTE_MAX ||
      input.y > LG_INPUT_MOUSE_ABSOLUTE_MAX ||
      input.wheel < LG_INPUT_MOUSE_WHEEL_MIN ||
      (input.buttons & ~LG_INPUT_MOUSE_BUTTON_MASK))
    return false;

  const HIDMouseAbsoluteReport report = {
    HID_REPORT_ID_MOUSE_ABSOLUTE,
    input.buttons,
    input.x,
    input.y,
    input.wheel,
  };
  return SubmitReport(&report, sizeof(report));
}

bool CInputPipeClient::HandleKeyboard(
  const void * payload,
  size_t size)
{
  if (size != sizeof(LGInputPipeKeyboard))
    return false;

  LGInputPipeKeyboard input = {};
  memcpy(&input, payload, sizeof(input));

  HIDKeyboardReport report = {};
  report.reportId = HID_REPORT_ID_KEYBOARD;
  report.modifiers = input.modifiers;
  for (size_t i = 0; i < LG_INPUT_KEYBOARD_KEY_COUNT; ++i)
  {
    if (input.keys[i] > LG_INPUT_KEYBOARD_USAGE_MAX)
      return false;
    report.keys[i] = input.keys[i];
  }

  return SubmitReport(&report, sizeof(report));
}

bool CInputPipeClient::SubmitReport(
  const void * report,
  size_t size)
{
  const NTSTATUS status = CHIDDevice::SubmitReport(report, size);

  if (status == STATUS_INVALID_PARAMETER)
    return false;

  if (status == STATUS_BUFFER_OVERFLOW)
    DEBUG_WARN("LGInput HID report queue is full; dropping a report");
  else if (!NT_SUCCESS(status) && status != STATUS_DEVICE_NOT_READY)
    DEBUG_WARN_HR(status, "Failed to submit an LGInput HID report");
  return true;
}
