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

#include "ipc/CPipeServer.h"
#include "CDebug.h"
#include "display/CDeviceContext.h"

CPipeServer g_pipe;

bool CPipeServer::Init()
{
  m_endpoint.SetHandler(this);
  return m_endpoint.Start(
    LG_PIPE_NAME,
    CPipeEndpoint::Mode::Server,
    sizeof(LGPipeMsg));
}

void CPipeServer::DeInit()
{
  m_endpoint.Stop();
}

void CPipeServer::OnPipeConnected()
{
  AcquireSRWLockExclusive(&m_queueLock);
  std::vector<LGPipeMsg> queued;
  queued.swap(m_queue);

  for (size_t i = 0; i < queued.size(); ++i)
    if (!m_endpoint.Send(&queued[i], sizeof(queued[i])))
    {
      for (; i < queued.size(); ++i)
        QueueMsgLocked(queued[i]);
      break;
    }
  ReleaseSRWLockExclusive(&m_queueLock);
}

bool CPipeServer::OnPipeMessage(const void * message, size_t size)
{
  if (size != sizeof(LGPipeMsg))
    return false;

  const LGPipeMsg & msg = *static_cast<const LGPipeMsg *>(message);
  if (msg.size != sizeof(msg))
    return false;

  switch (msg.type)
  {
    case LGPipeMsg::RELOADSETTINGS:
      HandleReloadSettings();
      return true;

    default:
      DEBUG_ERROR("Unknown message type %d", msg.type);
      return true;
  }
}

void CPipeServer::QueueMsgLocked(const LGPipeMsg & msg)
{
  for (LGPipeMsg & queued : m_queue)
    if (queued.type == msg.type)
    {
      queued = msg;
      return;
    }

  m_queue.push_back(msg);
}

void CPipeServer::WriteMsg(const LGPipeMsg & msg)
{
  AcquireSRWLockExclusive(&m_queueLock);
  if (!m_endpoint.Send(&msg, sizeof(msg)))
    QueueMsgLocked(msg);
  ReleaseSRWLockExclusive(&m_queueLock);
}

void CPipeServer::HandleReloadSettings()
{
  DEBUG_INFO("Reloading settings");

  AcquireSRWLockShared(&m_deviceContextLock);
  if (m_deviceContext)
    m_deviceContext->ReloadSettings();
  ReleaseSRWLockShared(&m_deviceContextLock);
}

void CPipeServer::SetDeviceContext(CDeviceContext * context)
{
  AcquireSRWLockExclusive(&m_deviceContextLock);
  m_deviceContext = context;
  ReleaseSRWLockExclusive(&m_deviceContextLock);
}

void CPipeServer::SetCursorPos(uint32_t x, uint32_t y)
{
  // do not send cursor messages if we are not connected or they will end up queued
  if (!m_endpoint.IsConnected())
    return;

  LGPipeMsg msg = {};
  msg.size       = sizeof(msg);
  msg.type       = LGPipeMsg::SETCURSORPOS;
  msg.curorPos.x = x;
  msg.curorPos.y = y;
  // Cursor position is transient. If the connection is lost during this
  // write, drop it instead of replaying stale coordinates after reconnect.
  m_endpoint.Send(&msg, sizeof(msg));
}

void CPipeServer::SetDisplayMode(
  uint32_t width, uint32_t height, uint32_t refreshMilliHz)
{
  LGPipeMsg msg = {};
  msg.size                = sizeof(msg);
  msg.type                = LGPipeMsg::SETDISPLAYMODE;
  msg.displayMode.width          = width;
  msg.displayMode.height         = height;
  msg.displayMode.refreshMilliHz = refreshMilliHz;
  WriteMsg(msg);
}

void CPipeServer::SetGPUStatus(bool software)
{
  LGPipeMsg msg = {};
  msg.size               = sizeof(msg);
  msg.type               = LGPipeMsg::GPUSTATUS;
  msg.gpuStatus.software = software;
  WriteMsg(msg);
}

void CPipeServer::ResolutionRejected(uint32_t width, uint32_t height,
  uint32_t requiredSizeMiB)
{
  LGPipeMsg msg = {};
  msg.size = sizeof(msg);
  msg.type = LGPipeMsg::RESOLUTIONREJECTED;
  msg.resolutionRejected.width = width;
  msg.resolutionRejected.height = height;
  msg.resolutionRejected.requiredSizeMiB = requiredSizeMiB;
  WriteMsg(msg);
}
