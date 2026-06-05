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

#include "CPipeServer.h"
#include "CDebug.h"

CPipeServer g_pipe;

bool CPipeServer::Init()
{
  _DeInit();

  m_pipe.Attach(CreateNamedPipeA(
    LG_PIPE_NAME,
    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
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

  m_signal.Attach(CreateEvent(NULL, TRUE, FALSE, NULL));
  if (!m_signal.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create pipe signal event");
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
  if (m_signal.IsValid())
    SetEvent(m_signal.Get());

  if (m_thread.IsValid())
  {
    WaitForSingleObject(m_thread.Get(), INFINITE);
    m_thread.Close();
  }

  if (m_pipe.IsValid())
  {
    FlushFileBuffers(m_pipe.Get());
    m_pipe.Close();
  }

  m_signal.Close();
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

  HandleT<EventTraits> ioEvent(CreateEvent(NULL, TRUE, FALSE, NULL));
  if (!ioEvent.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "Can't create event for overlapped I/O!");
    WaitForSingleObject(m_signal.Get(), 5000);
    return;
  }

  while(m_running)
  {
    m_connected = false;

    OVERLAPPED overlapped = { 0 };
    overlapped.hEvent = ioEvent.Get();

    if (!ConnectNamedPipe(m_pipe.Get(), &overlapped))
    {
      DWORD dwError = GetLastError();
      switch (dwError) {
      case ERROR_PIPE_CONNECTED:
        break;
      case ERROR_IO_PENDING:
      {
        HANDLE hWait[] = { ioEvent.Get(), m_signal.Get() };
        switch (WaitForMultipleObjects(2, hWait, FALSE, INFINITE))
        {
        case WAIT_OBJECT_0:
          break;
        case WAIT_OBJECT_0 + 1:
          DEBUG_INFO("Connect interrupted by signal");
          CancelIo(m_pipe.Get());
          WaitForSingleObject(ioEvent.Get(), INFINITE);
          continue;
        }
        break;
      }
      default:
        DEBUG_ERROR_HR(dwError, "Error connecting to the named pipe");
        goto end;
      }
    }

    DEBUG_TRACE("Client connected");

    m_connected = true;

    for (const auto& msg : m_queue)
      WriteMsg(msg);
    m_queue.clear();

    while (m_running && m_connected)
    {
      LGPipeMsg msg;

      if (!ReadFile(m_pipe.Get(), &msg, sizeof(msg), NULL, &overlapped))
      {
        DWORD dwError = GetLastError();
        if (dwError != ERROR_IO_PENDING)
        {
          DEBUG_ERROR_HR(dwError, "ReadFile Failed");
          break;
        }

        HANDLE hWait[] = { ioEvent.Get(), m_signal.Get() };
        switch (WaitForMultipleObjects(2, hWait, FALSE, INFINITE))
        {
        case WAIT_OBJECT_0:
          break;
        case WAIT_OBJECT_0 + 1:
          DEBUG_INFO("I/O interrupted by signal");
          CancelIo(m_pipe.Get());
          WaitForSingleObject(ioEvent.Get(), INFINITE);
          continue;
        }
      }

      DWORD bytesRead;
      GetOverlappedResult(m_pipe.Get(), &overlapped, &bytesRead, TRUE);

      if (bytesRead != sizeof(msg))
      {
        DEBUG_ERROR("Corrupted data, expected %lld bytes, read %lld bytes", sizeof msg, bytesRead);
        break;
      }

      if (msg.size != sizeof(msg))
      {
        DEBUG_ERROR("Corrupted data, expected %lld bytes, actual message size: %lld bytes", sizeof msg, msg.size);
        break;
      }

      switch (msg.type)
      {
      case LGPipeMsg::RELOADSETTINGS:
        HandleReloadSettings();
        break;

      default:
        DEBUG_ERROR("Unknown message type %d", msg.type);
        break;
      }
    }

    DEBUG_TRACE("Client disconnected");
    DisconnectNamedPipe(m_pipe.Get());

    if (m_running)
      ResetEvent(m_signal.Get());
  }

end:
  m_running   = false;
  m_connected = false;
  DEBUG_TRACE("Pipe thread shutdown");
}

void CPipeServer::WriteMsg(const LGPipeMsg & msg)
{
  if (!m_connected)
  {
    m_queue.push_back(msg);
    return;
  }

  DWORD written;
  if (!WriteFile(m_pipe.Get(), &msg, sizeof(msg), &written, NULL))
  {
    DWORD err = GetLastError();
    if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA)
    {
      DEBUG_WARN_HR(err, "Client disconnected, failed to write");
      m_connected = false;
      SetEvent(m_signal.Get());
      return;
    }

    DEBUG_WARN_HR(err, "WriteFile failed on the pipe");
    return;
  }

  FlushFileBuffers(m_pipe.Get());
}

void CPipeServer::HandleReloadSettings()
{
  DEBUG_INFO("TODO: reload settings");
}

void CPipeServer::SetCursorPos(uint32_t x, uint32_t y)
{
  // do not send cursor messages if we are not connected or they will end up queued
  if (!m_connected)
    return;

  LGPipeMsg msg;
  msg.size       = sizeof(msg);
  msg.type       = LGPipeMsg::SETCURSORPOS;
  msg.curorPos.x = x;
  msg.curorPos.y = y;
  WriteMsg(msg);
}

void CPipeServer::SetDisplayMode(uint32_t width, uint32_t height, uint32_t refresh)
{
  LGPipeMsg msg;
  msg.size                = sizeof(msg);
  msg.type                = LGPipeMsg::SETDISPLAYMODE;
  msg.displayMode.width   = width;
  msg.displayMode.height  = height;
  msg.displayMode.refresh = refresh;
  WriteMsg(msg);
}

void CPipeServer::SetGPUStatus(bool software)
{
  LGPipeMsg msg;
  msg.size               = sizeof(msg);
  msg.type               = LGPipeMsg::GPUSTATUS;
  msg.gpuStatus.software = software;
  WriteMsg(msg);
}