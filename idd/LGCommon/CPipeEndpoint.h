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

#pragma once

#include "CSRWLock.h"

#include <Windows.h>

#include <atomic>
#include <stddef.h>
#include <string>

class IPipeEndpointHandler
{
public:
  virtual ~IPipeEndpointHandler() = default;

  virtual void OnPipeConnected() {}
  virtual void OnPipeDisconnected() {}
  virtual bool ShouldReconnect() { return true; }
  virtual bool OnPipeMessage(
    _In_reads_bytes_(size) const void * message,
    _In_ size_t size) = 0;
};

class CPipeEndpoint
{
public:
  enum class Mode
  {
    Server,
    Client,
  };

  CPipeEndpoint() = default;
  ~CPipeEndpoint();

  CPipeEndpoint(const CPipeEndpoint &) = delete;
  CPipeEndpoint & operator=(const CPipeEndpoint &) = delete;

  bool Start(
    _In_z_ const wchar_t * pipeName,
    _In_ Mode mode,
    _In_ size_t messageSize);
  void Stop();

  bool Send(
    _In_reads_bytes_(size) const void * message,
    _In_ size_t size);

  bool IsRunning() const { return m_running.load(); }
  bool IsConnected() const { return m_connected.load(); }

  void SetHandler(_In_opt_ IPipeEndpointHandler * handler)
  {
    m_handler = handler;
  }

private:
  enum class PipeIoResult
  {
    Success,
    Disconnected,
    Stopped,
    Error,
  };

  static const DWORD CLIENT_RETRY_INITIAL_MS;
  static const DWORD CLIENT_RETRY_MAX_MS;
  static const DWORD SERVER_RETRY_MS;
  static const DWORD WRITE_TIMEOUT_MS;
  static const DWORD WAIT_FIRST_OBJECT_VALUE;

  static bool IsDisconnectedError(_In_ DWORD error);
  PipeIoResult WaitForOverlapped(
    _In_ HANDLE pipe,
    _In_ HANDLE ioEvent,
    _Inout_ OVERLAPPED * overlapped,
    _Out_ DWORD * transferred,
    _In_ DWORD timeoutMs = INFINITE);
  PipeIoResult ReadMessage(
    _In_ HANDLE pipe,
    _In_ HANDLE ioEvent,
    _Out_writes_bytes_(messageSize) void * message,
    _In_ DWORD messageSize,
    _Out_ DWORD * bytesRead);
  PipeIoResult WriteMessage(
    _In_ HANDLE pipe,
    _In_reads_bytes_(messageSize) const void * message,
    _In_ DWORD messageSize);

  static DWORD WINAPI ThreadProc(_In_ void * context);

  void Thread();
  void RunServer();
  void RunClient();
  bool ReadMessages(_In_ HANDLE pipe);
  HANDLE CreateServerPipe();
  bool WaitForRetry(_In_ DWORD delayMs);
  void PublishPipe(_In_ HANDLE pipe);
  void ClosePipe(_In_ HANDLE pipe);

  std::wstring m_pipeName;
  size_t m_messageSize = 0;
  Mode m_mode = Mode::Client;
  IPipeEndpointHandler * m_handler = nullptr;

  std::atomic<bool> m_running { false };
  std::atomic<bool> m_connected { false };

  CSRWLock m_pipeLock;
  HANDLE m_pipe = INVALID_HANDLE_VALUE;
  HANDLE m_thread = nullptr;
  HANDLE m_stopEvent = nullptr;
  HANDLE m_writeEvent = nullptr;
};
