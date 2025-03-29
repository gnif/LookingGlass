/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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

#include "CPipeServer.h"
#include "CDebug.h"

CPipeServer g_pipe;

bool CPipeServer::Init()
{
  _DeInit();

  m_pipe.Attach(CreateNamedPipeA(
    LG_PIPE_NAME,
    PIPE_ACCESS_DUPLEX,
    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
    1,
    1024,
    1024,
    0,
    NULL));

  if (!m_pipe.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create the named pipe");
    return false;
  }

  m_running = true;
  m_thread.Attach(CreateThread(
    NULL,
    0,
    _pipeThread,
    (LPVOID)this,
    0,
    NULL));

  if (!m_thread.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create the pipe thread");
    return false;
  }

  DEBUG_TRACE("Pipe Initialized");
  return true;
}

void CPipeServer::_DeInit()
{
  m_running = false;
  m_connected = false;

  if (m_thread.IsValid())
  {
    CancelSynchronousIo(m_thread.Get());
    WaitForSingleObject(m_thread.Get(), INFINITE);
    m_thread.Close();
  }

  if (m_pipe.IsValid())
  {
    FlushFileBuffers(m_pipe.Get());
    m_pipe.Close();
  }
}

void CPipeServer::DeInit()
{  
  DEBUG_TRACE("Pipe Stopping");
  _DeInit();
  DEBUG_TRACE("Pipe Stopped");
}

void CPipeServer::Thread()
{
  DEBUG_TRACE("Pipe thread started");
  while(m_running)
  {
    m_connected = false;
    bool result = ConnectNamedPipe(m_pipe.Get(), NULL);
    DWORD err = GetLastError();
    if (!result && err != ERROR_PIPE_CONNECTED)
    {
      // if graceful shutdown
      if ((err == ERROR_OPERATION_ABORTED && !m_running) ||
          err == ERROR_NO_DATA)
        break;

      // if timeout
      if (err == ERROR_SEM_TIMEOUT)
        continue;

      DEBUG_FATAL_HR(err, "Error connecting to the named pipe");
      break;
    }

    DEBUG_TRACE("Client connected");

    m_connected = true;
    while (m_running && m_connected)
    {
      //TODO: Read messages from the client
      Sleep(1000);
    }

    DEBUG_TRACE("Client disconnected");
    DisconnectNamedPipe(m_pipe.Get());
  }

  m_running   = false;
  m_connected = false;
  DEBUG_TRACE("Pipe thread shutdown");
}

void CPipeServer::WriteMsg(LGPipeMsg & msg)
{
  DWORD written;
  if (!WriteFile(m_pipe.Get(), &msg, sizeof(msg), &written, NULL))
  {
    DWORD err = GetLastError();
    if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA)
    {
      DEBUG_WARN_HR(err, "Client disconnected, failed to write");
      m_connected = false;
      return;
    }

    DEBUG_WARN_HR(err, "WriteFile failed on the pipe");
    return;
  }

  FlushFileBuffers(m_pipe.Get());
}

void CPipeServer::SetCursorPos(uint32_t x, uint32_t y)
{
  if (!m_connected)
    return;

  LGPipeMsg msg;
  msg.size       = sizeof(msg);
  msg.type       = LGPipeMsg::SETCURSORPOS;
  msg.curorPos.x = x;
  msg.curorPos.y = y;
  WriteMsg(msg);
}

void CPipeServer::SetDisplayMode(uint32_t width, uint32_t height)
{
  if (!m_connected)
    return;

  LGPipeMsg msg;
  msg.size               = sizeof(msg);
  msg.type               = LGPipeMsg::SETDISPLAYMODE;
  msg.displayMode.width  = width;
  msg.displayMode.height = height;
  WriteMsg(msg);
}