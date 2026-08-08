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
      message.type != LG_INPUT_PIPE_MESSAGE_REPORT ||
      !message.payloadSize ||
      message.payloadSize > sizeof(message.payload))
    return false;

  if (!message.sequence ||
      (m_lastSequence && message.sequence != m_lastSequence + 1))
  {
    DEBUG_WARN("LGInput pipe report sequence changed unexpectedly");
    return false;
  }
  m_lastSequence = message.sequence;

  const NTSTATUS status =
    CHIDDevice::SubmitReport(message.payload, message.payloadSize);
  if (status == STATUS_INVALID_PARAMETER)
    return false;

  if (status == STATUS_BUFFER_OVERFLOW)
    DEBUG_WARN("LGInput HID report queue is full; dropping a report");
  else if (!NT_SUCCESS(status) && status != STATUS_DEVICE_NOT_READY)
    DEBUG_WARN_HR(status, "Failed to submit an LGInput HID report");
  return true;
}
