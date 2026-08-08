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

bool CInputPipeServer::SendReport(const void * report, size_t size)
{
  if (!report || !size || size > LG_INPUT_PIPE_MAX_REPORT_SIZE)
    return false;

  LGInputPipeMessage message = {};
  message.magic = LG_INPUT_PIPE_MAGIC;
  message.version = LG_INPUT_PIPE_VERSION;
  message.type = LG_INPUT_PIPE_MESSAGE_REPORT;
  message.payloadSize = static_cast<uint32_t>(size);
  memcpy(message.payload, report, size);

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
