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

#include "CPipeEndpoint.h"

#include "CDebug.h"

#include <algorithm>
#include <stdint.h>
#include <vector>

const DWORD CPipeEndpoint::CLIENT_RETRY_INITIAL_MS = 100;
const DWORD CPipeEndpoint::CLIENT_RETRY_MAX_MS = 2000;
const DWORD CPipeEndpoint::SERVER_RETRY_MS = 1000;
const DWORD CPipeEndpoint::WRITE_TIMEOUT_MS = 250;
const DWORD CPipeEndpoint::WAIT_FIRST_OBJECT_VALUE = 0;

bool CPipeEndpoint::IsDisconnectedError(DWORD error)
{
  return error == ERROR_BROKEN_PIPE ||
    error == ERROR_NO_DATA ||
    error == ERROR_PIPE_NOT_CONNECTED;
}

CPipeEndpoint::PipeIoResult CPipeEndpoint::WaitForOverlapped(
  HANDLE pipe,
  HANDLE ioEvent,
  OVERLAPPED * overlapped,
  DWORD * transferred,
  DWORD timeoutMs)
{
  const HANDLE waitHandles[] = { ioEvent, m_stopEvent };
  const DWORD waitResult = WaitForMultipleObjects(
    _countof(waitHandles), waitHandles, FALSE, timeoutMs);

  if (waitResult == WAIT_TIMEOUT)
  {
    CancelIoEx(pipe, overlapped);
    if (GetOverlappedResult(pipe, overlapped, transferred, TRUE))
      return PipeIoResult::Success;
    DEBUG_WARN("Named pipe write timed out");
    return PipeIoResult::Error;
  }

  if (waitResult == WAIT_FIRST_OBJECT_VALUE + 1)
  {
    CancelIoEx(pipe, overlapped);
    GetOverlappedResult(pipe, overlapped, transferred, TRUE);
    return PipeIoResult::Stopped;
  }

  if (waitResult != WAIT_FIRST_OBJECT_VALUE)
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to wait for named pipe I/O");
    CancelIoEx(pipe, overlapped);
    GetOverlappedResult(pipe, overlapped, transferred, TRUE);
    return PipeIoResult::Error;
  }

  if (GetOverlappedResult(pipe, overlapped, transferred, FALSE))
    return PipeIoResult::Success;

  const DWORD error = GetLastError();
  if (error == ERROR_OPERATION_ABORTED &&
      WaitForSingleObject(m_stopEvent, 0) == WAIT_FIRST_OBJECT_VALUE)
    return PipeIoResult::Stopped;

  if (IsDisconnectedError(error))
    return PipeIoResult::Disconnected;

  DEBUG_WARN_HR(error, "Named pipe I/O failed");
  return PipeIoResult::Error;
}

CPipeEndpoint::PipeIoResult CPipeEndpoint::ReadMessage(
  HANDLE pipe,
  HANDLE ioEvent,
  void * message,
  DWORD messageSize,
  DWORD * bytesRead)
{
  ResetEvent(ioEvent);
  OVERLAPPED overlapped = {};
  overlapped.hEvent = ioEvent;

  if (ReadFile(pipe, message, messageSize, bytesRead, &overlapped))
    return PipeIoResult::Success;

  const DWORD error = GetLastError();
  if (error == ERROR_IO_PENDING)
    return WaitForOverlapped(
      pipe, ioEvent, &overlapped, bytesRead);

  if (error == ERROR_OPERATION_ABORTED &&
      WaitForSingleObject(m_stopEvent, 0) == WAIT_FIRST_OBJECT_VALUE)
    return PipeIoResult::Stopped;

  if (IsDisconnectedError(error))
    return PipeIoResult::Disconnected;

  if (error == ERROR_MORE_DATA)
  {
    DEBUG_ERROR("Named pipe message exceeds the negotiated frame size");
    return PipeIoResult::Error;
  }

  DEBUG_WARN_HR(error, "Failed to read from named pipe");
  return PipeIoResult::Error;
}

CPipeEndpoint::PipeIoResult CPipeEndpoint::WriteMessage(
  HANDLE pipe,
  const void * message,
  DWORD messageSize)
{
  ResetEvent(m_writeEvent);
  OVERLAPPED overlapped = {};
  overlapped.hEvent = m_writeEvent;
  DWORD bytesWritten = 0;
  PipeIoResult result = PipeIoResult::Success;

  if (!WriteFile(pipe, message, messageSize, &bytesWritten, &overlapped))
  {
    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING)
      result = WaitForOverlapped(
        pipe,
        m_writeEvent,
        &overlapped,
        &bytesWritten,
        WRITE_TIMEOUT_MS);
    else if (IsDisconnectedError(error))
      result = PipeIoResult::Disconnected;
    else if (error == ERROR_OPERATION_ABORTED &&
             WaitForSingleObject(m_stopEvent, 0) ==
               WAIT_FIRST_OBJECT_VALUE)
      result = PipeIoResult::Stopped;
    else
    {
      DEBUG_WARN_HR(error, "Failed to write to named pipe");
      result = PipeIoResult::Error;
    }
  }

  if (result == PipeIoResult::Success && bytesWritten != messageSize)
  {
    DEBUG_ERROR(
      "Short named pipe write, expected %lu bytes, wrote %lu bytes",
      messageSize,
      bytesWritten);
    result = PipeIoResult::Error;
  }

  return result;
}

CPipeEndpoint::~CPipeEndpoint()
{
  Stop();
}

bool CPipeEndpoint::Start(
  const wchar_t * pipeName,
  Mode mode,
  size_t messageSize)
{
  Stop();

  if (!pipeName || !*pipeName || !messageSize || messageSize > MAXDWORD)
    return false;

  m_pipeName = pipeName;
  m_mode = mode;
  m_messageSize = messageSize;
  m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  m_writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!m_stopEvent || !m_writeEvent)
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create named pipe events");
    if (m_writeEvent)
      CloseHandle(m_writeEvent);
    if (m_stopEvent)
      CloseHandle(m_stopEvent);
    m_writeEvent = nullptr;
    m_stopEvent  = nullptr;
    return false;
  }

  if (m_mode == Mode::Server)
  {
    HANDLE pipe = CreateServerPipe();
    if (pipe == INVALID_HANDLE_VALUE)
    {
      CloseHandle(m_stopEvent);
      m_stopEvent = nullptr;
      CloseHandle(m_writeEvent);
      m_writeEvent = nullptr;
      return false;
    }
    PublishPipe(pipe);
  }

  m_running.store(true);
  m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
  if (!m_thread)
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create named pipe thread");
    m_running.store(false);

    AcquireSRWLockExclusive(&m_pipeLock);
    if (m_pipe != INVALID_HANDLE_VALUE)
    {
      CloseHandle(m_pipe);
      m_pipe = INVALID_HANDLE_VALUE;
    }
    ReleaseSRWLockExclusive(&m_pipeLock);

    CloseHandle(m_stopEvent);
    m_stopEvent = nullptr;
    CloseHandle(m_writeEvent);
    m_writeEvent = nullptr;
    return false;
  }

  return true;
}

void CPipeEndpoint::Stop()
{
  m_running.store(false);
  m_connected.store(false);
  if (m_stopEvent)
    SetEvent(m_stopEvent);

  AcquireSRWLockShared(&m_pipeLock);
  if (m_pipe != INVALID_HANDLE_VALUE)
    CancelIoEx(m_pipe, nullptr);
  ReleaseSRWLockShared(&m_pipeLock);

  if (m_thread)
  {
    WaitForSingleObject(m_thread, INFINITE);
    CloseHandle(m_thread);
    m_thread = nullptr;
  }

  AcquireSRWLockExclusive(&m_pipeLock);
  if (m_pipe != INVALID_HANDLE_VALUE)
  {
    CloseHandle(m_pipe);
    m_pipe = INVALID_HANDLE_VALUE;
  }
  ReleaseSRWLockExclusive(&m_pipeLock);

  if (m_stopEvent)
  {
    CloseHandle(m_stopEvent);
    m_stopEvent = nullptr;
  }

  if (m_writeEvent)
  {
    CloseHandle(m_writeEvent);
    m_writeEvent = nullptr;
  }

  m_connected.store(false);
}

bool CPipeEndpoint::Send(const void * message, size_t size)
{
  if (!message || size != m_messageSize || !IsRunning() || !IsConnected())
    return false;

  bool success = false;
  AcquireSRWLockExclusive(&m_pipeLock);
  if (m_pipe != INVALID_HANDLE_VALUE && IsConnected())
  {
    const PipeIoResult result = WriteMessage(
      m_pipe,
      message,
      static_cast<DWORD>(size));
    success = result == PipeIoResult::Success;
    if (!success)
    {
      m_connected.store(false);
      CancelIoEx(m_pipe, nullptr);
    }
  }
  ReleaseSRWLockExclusive(&m_pipeLock);
  return success;
}

DWORD WINAPI CPipeEndpoint::ThreadProc(void * context)
{
  static_cast<CPipeEndpoint *>(context)->Thread();
  return 0;
}

void CPipeEndpoint::Thread()
{
  if (m_mode == Mode::Server)
    RunServer();
  else
    RunClient();

  m_running.store(false);
  m_connected.store(false);
}

HANDLE CPipeEndpoint::CreateServerPipe()
{
  const DWORD bufferSize = static_cast<DWORD>(
    std::max<size_t>(m_messageSize, 1024));

  HANDLE pipe = CreateNamedPipeW(
    m_pipeName.c_str(),
    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
    1,
    bufferSize,
    bufferSize,
    0,
    nullptr);
  if (pipe == INVALID_HANDLE_VALUE)
    DEBUG_ERROR_HR(GetLastError(), "Failed to create named pipe %ls", m_pipeName.c_str());
  return pipe;
}

void CPipeEndpoint::RunServer()
{
  HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ioEvent)
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create named pipe I/O event");
    AcquireSRWLockShared(&m_pipeLock);
    const HANDLE pipe = m_pipe;
    ReleaseSRWLockShared(&m_pipeLock);
    if (pipe != INVALID_HANDLE_VALUE)
      ClosePipe(pipe);
    return;
  }

  HANDLE pipe = INVALID_HANDLE_VALUE;
  AcquireSRWLockShared(&m_pipeLock);
  pipe = m_pipe;
  ReleaseSRWLockShared(&m_pipeLock);

  while (IsRunning())
  {
    if (pipe == INVALID_HANDLE_VALUE)
    {
      pipe = CreateServerPipe();
      if (pipe == INVALID_HANDLE_VALUE)
      {
        if (!WaitForRetry(SERVER_RETRY_MS))
          break;
        continue;
      }
      PublishPipe(pipe);
    }

    ResetEvent(ioEvent);
    OVERLAPPED overlapped = {};
    overlapped.hEvent = ioEvent;
    DWORD transferred = 0;
    PipeIoResult connectResult = PipeIoResult::Success;

    if (!ConnectNamedPipe(pipe, &overlapped))
    {
      const DWORD error = GetLastError();
      if (error == ERROR_PIPE_CONNECTED)
        connectResult = PipeIoResult::Success;
      else if (error == ERROR_IO_PENDING)
        connectResult = WaitForOverlapped(
          pipe, ioEvent, &overlapped, &transferred);
      else if (error == ERROR_OPERATION_ABORTED && !IsRunning())
        connectResult = PipeIoResult::Stopped;
      else
      {
        DEBUG_WARN_HR(error, "Failed to accept named pipe client");
        connectResult = PipeIoResult::Error;
      }
    }

    if (connectResult != PipeIoResult::Success)
    {
      ClosePipe(pipe);
      pipe = INVALID_HANDLE_VALUE;
      if (connectResult == PipeIoResult::Stopped || !IsRunning())
        break;
      continue;
    }

    if (!IsRunning())
      break;

    if (!IsRunning() ||
        WaitForSingleObject(m_stopEvent, 0) == WAIT_FIRST_OBJECT_VALUE)
      break;

    m_connected.store(true);
    DEBUG_INFO("Named pipe client connected: %ls", m_pipeName.c_str());
    if (m_handler)
      m_handler->OnPipeConnected();

    ReadMessages(pipe);

    m_connected.store(false);
    if (m_handler)
      m_handler->OnPipeDisconnected();
    DEBUG_INFO("Named pipe client disconnected: %ls", m_pipeName.c_str());

    if (!DisconnectNamedPipe(pipe))
    {
      const DWORD error = GetLastError();
      if (error != ERROR_PIPE_NOT_CONNECTED)
      {
        DEBUG_WARN_HR(error, "Failed to disconnect named pipe client");
        ClosePipe(pipe);
        pipe = INVALID_HANDLE_VALUE;
      }
    }
  }

  if (pipe != INVALID_HANDLE_VALUE)
    ClosePipe(pipe);
  CloseHandle(ioEvent);
}

void CPipeEndpoint::RunClient()
{
  DWORD retryDelay = CLIENT_RETRY_INITIAL_MS;
  DWORD lastConnectError = ERROR_SUCCESS;

  while (IsRunning())
  {
    if (m_handler && !m_handler->ShouldReconnect())
      break;

    HANDLE pipe = CreateFileW(
      m_pipeName.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OVERLAPPED,
      nullptr);

    if (pipe == INVALID_HANDLE_VALUE)
    {
      const DWORD error = GetLastError();
      if (error != lastConnectError)
      {
        DEBUG_TRACE_HR(
          error, "Named pipe is not available yet: %ls", m_pipeName.c_str());
        lastConnectError = error;
      }

      if (!WaitForRetry(retryDelay))
        break;
      retryDelay = (std::min)(retryDelay * 2, CLIENT_RETRY_MAX_MS);
      continue;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr))
    {
      DEBUG_WARN_HR(GetLastError(), "Failed to set named pipe message mode");
      CloseHandle(pipe);
      if (!WaitForRetry(retryDelay))
        break;
      retryDelay = (std::min)(retryDelay * 2, CLIENT_RETRY_MAX_MS);
      continue;
    }

    if (!IsRunning() ||
        WaitForSingleObject(m_stopEvent, 0) == WAIT_FIRST_OBJECT_VALUE)
    {
      CloseHandle(pipe);
      break;
    }

    PublishPipe(pipe);
    m_connected.store(true);
    retryDelay = CLIENT_RETRY_INITIAL_MS;
    lastConnectError = ERROR_SUCCESS;
    DEBUG_INFO("Named pipe connected: %ls", m_pipeName.c_str());
    if (m_handler)
      m_handler->OnPipeConnected();

    ReadMessages(pipe);

    m_connected.store(false);
    if (m_handler)
      m_handler->OnPipeDisconnected();
    DEBUG_INFO("Named pipe disconnected: %ls", m_pipeName.c_str());
    ClosePipe(pipe);

    if (!WaitForRetry(retryDelay))
      break;
  }
}

bool CPipeEndpoint::ReadMessages(HANDLE pipe)
{
  HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ioEvent)
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create named pipe read event");
    return false;
  }

  std::vector<uint8_t> message(m_messageSize);
  bool success = true;
  while (IsRunning() && IsConnected())
  {
    DWORD bytesRead = 0;
    const PipeIoResult result = ReadMessage(
      pipe,
      ioEvent,
      message.data(),
      static_cast<DWORD>(message.size()),
      &bytesRead);
    if (result != PipeIoResult::Success)
    {
      success = result == PipeIoResult::Disconnected ||
        result == PipeIoResult::Stopped;
      break;
    }

    if (bytesRead != message.size())
    {
      DEBUG_ERROR(
        "Invalid named pipe frame size, expected %llu bytes, received %lu",
        static_cast<unsigned long long>(message.size()),
        bytesRead);
      success = false;
      break;
    }

    if (!IsRunning() ||
        WaitForSingleObject(m_stopEvent, 0) == WAIT_FIRST_OBJECT_VALUE)
      break;

    if (m_handler && !m_handler->OnPipeMessage(
          message.data(), message.size()))
    {
      DEBUG_ERROR("Named pipe peer sent an invalid message");
      success = false;
      break;
    }
  }

  CloseHandle(ioEvent);
  return success;
}

bool CPipeEndpoint::WaitForRetry(DWORD delayMs)
{
  return IsRunning() &&
    WaitForSingleObject(m_stopEvent, delayMs) == WAIT_TIMEOUT;
}

void CPipeEndpoint::PublishPipe(HANDLE pipe)
{
  AcquireSRWLockExclusive(&m_pipeLock);
  m_pipe = pipe;
  ReleaseSRWLockExclusive(&m_pipeLock);
}

void CPipeEndpoint::ClosePipe(HANDLE pipe)
{
  AcquireSRWLockExclusive(&m_pipeLock);
  if (m_pipe == pipe)
    m_pipe = INVALID_HANDLE_VALUE;
  CloseHandle(pipe);
  ReleaseSRWLockExclusive(&m_pipeLock);
}
