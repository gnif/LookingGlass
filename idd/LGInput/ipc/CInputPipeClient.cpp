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

static constexpr uint8_t HID_KEYBOARD_USAGE_MUTE        = 0x7f;
static constexpr uint8_t HID_KEYBOARD_USAGE_VOLUME_UP   = 0x80;
static constexpr uint8_t HID_KEYBOARD_USAGE_VOLUME_DOWN = 0x81;

static constexpr uint16_t HID_CONSUMER_USAGE_MUTE        = 0xe2;
static constexpr uint16_t HID_CONSUMER_USAGE_VOLUME_UP   = 0xe9;
static constexpr uint16_t HID_CONSUMER_USAGE_VOLUME_DOWN = 0xea;

bool CInputPipeClient::Start()
{
  m_endpoint.Stop();
  m_lastSequence       = 0;
  m_statReceived       = 0;
  m_statMalformed      = 0;
  m_statSequenceResets = 0;
  m_statSubmitFailed   = 0;
  m_lastStatistics     = GetTickCount64();
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
  {
    LogStatistics(true);
    CHIDDevice::ResetReports();
  }
}

void CInputPipeClient::OnPipeConnected()
{
  m_lastSequence = 0;
  DEBUG_INFO("Connected to the LGIdd input transport");
}

void CInputPipeClient::OnPipeDisconnected()
{
  m_lastSequence = 0;
  LogStatistics(true);
  CHIDDevice::ResetReports();
  DEBUG_INFO("Disconnected from the LGIdd input transport; reconnecting");
}

bool CInputPipeClient::OnPipeMessage(
  const void * frame,
  size_t size)
{
  if (size != sizeof(LGInputPipeMessage))
  {
    ++m_statMalformed;
    DEBUG_WARN("Received a malformed LGInput pipe message");
    return false;
  }

  const LGInputPipeMessage & message =
    *static_cast<const LGInputPipeMessage *>(frame);
  if (message.magic != LG_INPUT_PIPE_MAGIC ||
      message.version != LG_INPUT_PIPE_VERSION ||
      !message.payloadSize ||
      message.payloadSize > sizeof(message.payload))
  {
    ++m_statMalformed;
    DEBUG_WARN("Received a malformed LGInput pipe message");
    return false;
  }

  if (!message.sequence ||
      (m_lastSequence && message.sequence != m_lastSequence + 1))
  {
    ++m_statSequenceResets;
    DEBUG_WARN("LGInput pipe report sequence changed unexpectedly");
    CHIDDevice::ResetReports();
    return false;
  }
  bool handled = false;
  const uint64_t submitFailures = m_statSubmitFailed;
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
      ++m_statMalformed;
      DEBUG_WARN("Received an unknown LGInput pipe message");
      return false;
  }

  if (!handled)
  {
    if (m_statSubmitFailed == submitFailures)
    {
      ++m_statMalformed;
      DEBUG_WARN("Received a malformed LGInput pipe payload");
    }
    return false;
  }

  m_lastSequence = message.sequence;
  ++m_statReceived;
  LogStatistics(false);
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
  if (input.deltaX < LG_INPUT_MOUSE_DELTA_MIN ||
      input.deltaX > LG_INPUT_MOUSE_DELTA_MAX ||
      input.deltaY < LG_INPUT_MOUSE_DELTA_MIN ||
      input.deltaY > LG_INPUT_MOUSE_DELTA_MAX ||
      input.wheel < LG_INPUT_MOUSE_WHEEL_MIN_TOTAL ||
      input.wheel > LG_INPUT_MOUSE_WHEEL_MAX)
    return false;

  int32_t x     = input.deltaX;
  int32_t y     = input.deltaY;
  int32_t wheel = input.wheel;
  do
  {
    const int16_t reportX = x > INT16_MAX ? INT16_MAX :
      x < INT16_MIN ? INT16_MIN : static_cast<int16_t>(x);
    const int16_t reportY = y > INT16_MAX ? INT16_MAX :
      y < INT16_MIN ? INT16_MIN : static_cast<int16_t>(y);
    const int8_t reportWheel = wheel > INT8_MAX ? INT8_MAX :
      wheel < LG_INPUT_MOUSE_WHEEL_MIN ? LG_INPUT_MOUSE_WHEEL_MIN :
      static_cast<int8_t>(wheel);
    const HIDMouseRelativeReport report = {
      HID_REPORT_ID_MOUSE_RELATIVE,
      input.buttons,
      reportX,
      reportY,
      reportWheel,
    };
    if (!SubmitReport(&report, sizeof(report)))
      return false;
    x     -= reportX;
    y     -= reportY;
    wheel -= reportWheel;
  }
  while (x || y || wheel);
  return true;
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
      input.wheel < LG_INPUT_MOUSE_WHEEL_MIN_TOTAL ||
      input.wheel > LG_INPUT_MOUSE_WHEEL_MAX ||
      input.reserved)
    return false;

  int32_t wheel = input.wheel;
  do
  {
    const int8_t reportWheel = wheel > INT8_MAX ? INT8_MAX :
      wheel < LG_INPUT_MOUSE_WHEEL_MIN ? LG_INPUT_MOUSE_WHEEL_MIN :
      static_cast<int8_t>(wheel);
    const HIDMouseAbsoluteReport report = {
      HID_REPORT_ID_MOUSE_ABSOLUTE,
      input.buttons,
      input.x,
      input.y,
      reportWheel,
    };
    if (!SubmitReport(&report, sizeof(report)))
      return false;
    wheel -= reportWheel;
  }
  while (wheel);
  return true;
}

bool CInputPipeClient::HandleKeyboard(
  const void * payload,
  size_t size)
{
  if (size != sizeof(LGInputPipeKeyboard))
    return false;

  LGInputPipeKeyboard input = {};
  memcpy(&input, payload, sizeof(input));

  for (size_t i = 0; i < sizeof(input.reserved); ++i)
    if (input.reserved[i])
      return false;

  HIDKeyboardReport report = {};
  report.reportId = HID_REPORT_ID_KEYBOARD;
  report.modifiers = input.modifiers;
  // The Consumer Control report carries one active array usage.
  uint16_t consumerUsage = 0;
  size_t   keyIndex      = 0;
  for (size_t i = 0; i < LG_INPUT_KEYBOARD_KEY_COUNT; ++i)
  {
    if (input.keys[i] > LG_INPUT_KEYBOARD_USAGE_MAX)
      return false;

    switch (input.keys[i])
    {
      case HID_KEYBOARD_USAGE_MUTE:
        if (!consumerUsage)
          consumerUsage = HID_CONSUMER_USAGE_MUTE;
        break;

      case HID_KEYBOARD_USAGE_VOLUME_UP:
        if (!consumerUsage)
          consumerUsage = HID_CONSUMER_USAGE_VOLUME_UP;
        break;

      case HID_KEYBOARD_USAGE_VOLUME_DOWN:
        if (!consumerUsage)
          consumerUsage = HID_CONSUMER_USAGE_VOLUME_DOWN;
        break;

      default:
        report.keys[keyIndex++] = input.keys[i];
        break;
    }
  }

  if (!SubmitReport(&report, sizeof(report)))
    return false;

  const HIDConsumerReport consumer = {
    HID_REPORT_ID_CONSUMER,
    consumerUsage,
  };
  return SubmitReport(&consumer, sizeof(consumer));
}

bool CInputPipeClient::SubmitReport(
  const void * report,
  size_t size)
{
  const NTSTATUS status = CHIDDevice::SubmitReport(report, size);

  if (status == STATUS_INVALID_PARAMETER)
  {
    ++m_statSubmitFailed;
    return false;
  }

  if (status == STATUS_BUFFER_OVERFLOW)
  {
    ++m_statSubmitFailed;
    DEBUG_WARN("LGInput HID report queue overflowed; resetting input state");
    CHIDDevice::ResetReports();
    return false;
  }
  if (!NT_SUCCESS(status))
  {
    ++m_statSubmitFailed;
    if (status != STATUS_DEVICE_NOT_READY)
      DEBUG_WARN_HR(status, "Failed to submit an LGInput HID report");
    return false;
  }
  return true;
}

void CInputPipeClient::LogStatistics(bool force)
{
  const ULONGLONG now = GetTickCount64();
  if (!force && now - m_lastStatistics < STATISTICS_INTERVAL_MS)
    return;

  const uint64_t received       = m_statReceived;
  const uint64_t malformed      = m_statMalformed;
  const uint64_t sequenceResets = m_statSequenceResets;
  const uint64_t submitFailed   = m_statSubmitFailed;

  m_statReceived       = 0;
  m_statMalformed      = 0;
  m_statSequenceResets = 0;
  m_statSubmitFailed   = 0;
  m_lastStatistics     = now;

  if (received || malformed || sequenceResets || submitFailed)
  {
    DEBUG_TRACE("LGInput pipe receive: %llu reports, %llu malformed, "
      "%llu sequence resets, %llu HID submit failures",
      static_cast<unsigned long long>(received),
      static_cast<unsigned long long>(malformed),
      static_cast<unsigned long long>(sequenceResets),
      static_cast<unsigned long long>(submitFailed));
  }
  CHIDDevice::LogStatistics();
}
