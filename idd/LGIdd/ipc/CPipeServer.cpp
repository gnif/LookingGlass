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
#include "CSRWLock.h"
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
  CSRWExclusiveLock lock(m_queueLock);
  std::vector<LGPipeMsg> queued;
  queued.swap(m_queue);

  for (size_t i = 0; i < queued.size(); ++i)
    if (!m_endpoint.Send(&queued[i], sizeof(queued[i])))
    {
      for (; i < queued.size(); ++i)
        QueueMsgLocked(queued[i]);
      break;
    }

  // Recovery is latched state rather than a one-shot command. Reapply the
  // latest request whenever the helper reconnects so a helper restart cannot
  // silently restore the IDD-only topology while recovery is active.
  if (m_recoveryValid)
    m_endpoint.Send(&m_recoveryRequest, sizeof(m_recoveryRequest));
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

    case LGPipeMsg::RECOVERY_OFF:
    case LGPipeMsg::RECOVERY_ON:
    case LGPipeMsg::RECOVERY_FAILED:
    case LGPipeMsg::RECOVERY_NO_DISPLAY:
      HandleRecovery(msg);
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
  CSRWExclusiveLock lock(m_queueLock);
  if (!m_endpoint.Send(&msg, sizeof(msg)))
    QueueMsgLocked(msg);
}

void CPipeServer::HandleReloadSettings()
{
  DEBUG_INFO("Reloading settings");

  CSRWSharedLock lock(m_deviceContextLock);
  if (m_deviceContext)
    m_deviceContext->ReloadSettings();
}

void CPipeServer::HandleRecovery(const LGPipeMsg & msg)
{
  CSRWSharedLock queueLock(m_queueLock);
  if (!m_recoveryValid ||
      msg.recovery.session != m_recoveryRequest.recovery.session ||
      msg.recovery.request != m_recoveryRequest.recovery.request)
  {
    DEBUG_WARN("Ignoring stale recovery status");
    return;
  }

  const uint32_t serial =
    msg.recovery.request & ~LGPipeMsg::RECOVERY_ACTIVE;
  const bool     active =
    (msg.recovery.request & LGPipeMsg::RECOVERY_ACTIVE) != 0;

  CSRWSharedLock recoveryLock(m_recoveryLock);
  queueLock.Unlock();
  if (m_recoveryHandler)
    m_recoveryHandler(m_recoveryOpaque,
      m_recoveryRoute, msg.recovery.session, serial, active, msg.type);
}

void CPipeServer::SetDeviceContext(CDeviceContext * context)
{
  CSRWExclusiveLock lock(m_deviceContextLock);
  m_deviceContext = context;
}

void CPipeServer::SetRecoveryHandler(
  RecoveryHandler handler, void * opaque)
{
  CSRWExclusiveLock queueLock(m_queueLock);
  CSRWExclusiveLock recoveryLock(m_recoveryLock);
  m_recoveryRoute   = 0;
  m_recoveryValid   = false;
  m_recoveryRequest = {};
  m_recoveryHandler = handler;
  m_recoveryOpaque  = opaque;
}

void CPipeServer::ClearRecoveryHandler(void * opaque)
{
  CSRWExclusiveLock queueLock(m_queueLock);
  CSRWExclusiveLock recoveryLock(m_recoveryLock);
  if (m_recoveryOpaque != opaque)
    return;

  m_recoveryRoute   = 0;
  m_recoveryValid   = false;
  m_recoveryRequest = {};
  m_recoveryHandler = nullptr;
  m_recoveryOpaque  = nullptr;
}

bool CPipeServer::SetCursorPos(int32_t x, int32_t y)
{
  // do not send cursor messages if we are not connected or they will end up queued
  if (!m_endpoint.IsConnected())
    return false;

  LGPipeMsg msg = {};
  msg.size       = sizeof(msg);
  msg.type       = LGPipeMsg::SETCURSORPOS;
  msg.curorPos.x = x;
  msg.curorPos.y = y;
  // Cursor position is transient. If the connection is lost during this
  // write, drop it instead of replaying stale coordinates after reconnect.
  return m_endpoint.Send(&msg, sizeof(msg));
}

void CPipeServer::SetDisplayMode(
  uint32_t width, uint32_t height, uint32_t refreshMilliHz)
{
  LGPipeMsg msg = {};
  msg.size                       = sizeof(msg);
  msg.type                       = LGPipeMsg::SETDISPLAYMODE;
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
  msg.size                               = sizeof(msg);
  msg.type                               = LGPipeMsg::RESOLUTIONREJECTED;
  msg.resolutionRejected.width           = width;
  msg.resolutionRejected.height          = height;
  msg.resolutionRejected.requiredSizeMiB = requiredSizeMiB;
  WriteMsg(msg);
}

bool CPipeServer::SetRecovery(void * owner, uint64_t route,
  uint64_t session, uint32_t serial, bool active)
{
  if (!route || !session || !serial ||
      (serial & LGPipeMsg::RECOVERY_ACTIVE))
  {
    DEBUG_ERROR("Invalid recovery request correlation");
    return false;
  }

  LGPipeMsg msg = {};
  msg.size             = sizeof(msg);
  msg.type             = LGPipeMsg::SET_RECOVERY;
  msg.recovery.session = session;
  msg.recovery.request = serial |
    (active ? LGPipeMsg::RECOVERY_ACTIVE : 0U);

  CSRWExclusiveLock queueLock(m_queueLock);
  CSRWExclusiveLock recoveryLock(m_recoveryLock);
  if (!m_recoveryHandler || m_recoveryOpaque != owner)
    return false;

  m_recoveryValid   = true;
  m_recoveryRoute   = route;
  m_recoveryRequest = msg;
  m_endpoint.Send(&msg, sizeof(msg));
  return true;
}
