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

#include "CPipeClient.h"
#include "CDebug.h"

#include <setupapi.h>
#include <tchar.h>

CPipeClient g_pipe;

bool CPipeClient::Init()
{
  DeInit();

  if (!IsLGIddDeviceAttached())
  {
    DEBUG_ERROR("Looking Glass Indirect Display Device not found");
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

  return true;
}

void CPipeClient::DeInit()
{
  m_connected = false;
  m_running = false;
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

bool CPipeClient::IsLGIddDeviceAttached()
{
  HDEVINFO hDevInfo = SetupDiGetClassDevs(
    NULL,
    NULL,
    NULL,
    DIGCF_ALLCLASSES | DIGCF_PRESENT
  );

  if (hDevInfo == INVALID_HANDLE_VALUE)
    return false;

  SP_DEVINFO_DATA DeviceInfoData;
  DeviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
  bool found = false;

  for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &DeviceInfoData); i++)
  {
    DWORD DataT;
    TCHAR buffer[1024];
    DWORD buffersize = 0;

    if (!SetupDiGetDeviceRegistryProperty(
      hDevInfo,
      &DeviceInfoData,
      SPDRP_HARDWAREID,
      &DataT,
      (PBYTE)buffer,
      sizeof(buffer),
      &buffersize))
      continue;

    for (LPCTSTR p = buffer; *p; p += _tcslen(p) + 1)
      if (_tcsicmp(p, _T("Root\\LGIdd")) == 0)
      {
        found = true;
        break;
      }

    if (found)
      break;
  }

  SetupDiDestroyDeviceInfoList(hDevInfo);
  return found;
}

/* APIs like SetCursorPos are applied to the desktop our thread is
 * attached to. If the user switches to the secure desktop (UAC, etc)
 * then these functions will not work, so call this first to ensure
 * the call is effective */
void CPipeClient::SetActiveDesktop()
{
  HDESK desktop = NULL;
  desktop = OpenInputDesktop(0, FALSE, GENERIC_READ);
  if (!desktop)
    DEBUG_ERROR_HR(GetLastError(), "OpenInputDesktop Failed");
  else
  {
    if (!SetThreadDesktop(desktop))
      DEBUG_ERROR_HR(GetLastError(), "SetThreadDesktop Failed");
    CloseDesktop(desktop);
  }
}

void CPipeClient::WriteMsg(const LGPipeMsg& msg)
{
  DWORD written;
  if (!WriteFile(m_pipe.Get(), &msg, sizeof(msg), &written, NULL))
  {
    DWORD err = GetLastError();
    if (err == ERROR_BROKEN_PIPE)
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

void CPipeClient::Thread()
{
  DEBUG_INFO("Pipe thread started");

  HandleT<EventTraits> ioEvent(CreateEvent(NULL, TRUE, FALSE, NULL));
  if (!ioEvent.IsValid())
  {
    DEBUG_ERROR("Can't create event for overlapped I/O!");
    WaitForSingleObject(m_signal.Get(), 5000);
    return;
  }

  while (m_running)
  {
    if (!IsLGIddDeviceAttached())
    {
      m_running = false;
      DEBUG_ERROR("Device is no longer available, shutting down");
      break;
    }

    m_pipe.Attach(CreateFile(
      TEXT(LG_PIPE_NAME),
      GENERIC_READ | GENERIC_WRITE,
      0,
      NULL,
      OPEN_EXISTING,
      FILE_FLAG_OVERLAPPED,
      NULL
    ));

    if (!m_pipe.IsValid())
    {
      DEBUG_ERROR_HR(GetLastError(), "Failed to open the named pipe");
      WaitForSingleObject(m_signal.Get(), 5000);
      continue;
    }

    m_connected = true;
    DEBUG_INFO("Pipe connected");

    while (m_running && m_connected)
    {
      LGPipeMsg msg;

      OVERLAPPED overlapped = { 0 };
      overlapped.hEvent = ioEvent.Get();

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
        case LGPipeMsg::SETCURSORPOS:
          HandleSetCursorPos(msg);
          break;

        case LGPipeMsg::SETDISPLAYMODE:
          HandleSetDisplayMode(msg);
          break;

        default:
          DEBUG_ERROR("Unknown message type %d", msg.type);
          break;
      }
    }

    m_pipe.Close();
    m_connected = false;
    DEBUG_INFO("Pipe closed");

    if (m_running)
      ResetEvent(m_signal.Get());
  }

  DEBUG_INFO("Pipe thread shutdown");
}

void CPipeClient::HandleSetCursorPos(const LGPipeMsg& msg)
{
  SetActiveDesktop();
  SetCursorPos(msg.curorPos.x, msg.curorPos.y);
}

void CPipeClient::HandleSetDisplayMode(const LGPipeMsg& msg)
{
  DEVMODE dm = {};
  dm.dmSize       = sizeof(dm);
  dm.dmPelsWidth  = msg.displayMode.width;
  dm.dmPelsHeight = msg.displayMode.height;
  dm.dmFields     = DM_PELSWIDTH | DM_PELSHEIGHT;

  LONG result = ChangeDisplaySettingsEx(NULL, &dm, NULL, CDS_UPDATEREGISTRY, NULL);
  if (result != DISP_CHANGE_SUCCESSFUL)
    DEBUG_ERROR("ChangeDisplaySettingsEx Failed (0x%08x)", result);
}
