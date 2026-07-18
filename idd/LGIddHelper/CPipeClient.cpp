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

#include "CPipeClient.h"
#include "CDebug.h"
#include "CNotifyWindow.h"

#include <setupapi.h>
#include <tchar.h>
#include <vector>

namespace
{
  struct DisplayState
  {
    DISPLAY_DEVICE device;
    DEVMODE mode;
    bool isLG;
  };

  bool IsLGDisplay(const DISPLAY_DEVICE& device)
  {
    static const TCHAR deviceId[] = _T("ROOT\\LGIDD");
    const size_t deviceIdLength = _countof(deviceId) - 1;

    if (_tcsnicmp(device.DeviceID, deviceId, deviceIdLength) == 0 &&
      (device.DeviceID[deviceIdLength] == _T('\\') ||
       device.DeviceID[deviceIdLength] == _T('\0')))
      return true;

    if (_tcsicmp(device.DeviceString,
      _T("Looking Glass Indirect Display Device")) == 0)
      return true;

    DISPLAY_DEVICE monitor = {};
    monitor.cb = sizeof(monitor);
    for (DWORD i = 0; EnumDisplayDevices(device.DeviceName, i, &monitor, 0); ++i)
    {
      if (_tcsnicmp(monitor.DeviceID, _T("MONITOR\\LGD1DDD"), 15) == 0 ||
        _tcsicmp(monitor.DeviceString, _T("Looking Glass")) == 0)
        return true;

      monitor = {};
      monitor.cb = sizeof(monitor);
    }

    return false;
  }

  bool GetDisplayStates(std::vector<DisplayState>& displays, size_t& lgIndex)
  {
    lgIndex = SIZE_MAX;

    DISPLAY_DEVICE device = {};
    device.cb = sizeof(device);
    for (DWORD i = 0; EnumDisplayDevices(NULL, i, &device, 0); ++i)
    {
      if ((device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) &&
        !(device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER))
      {
        DisplayState state = {};
        state.device = device;
        state.mode.dmSize = sizeof(state.mode);
        state.isLG = IsLGDisplay(device);

        if (!EnumDisplaySettingsEx(device.DeviceName, ENUM_CURRENT_SETTINGS,
          &state.mode, 0))
        {
          DEBUG_WARN_HR(GetLastError(),
            "Failed to query the current mode for %ls", device.DeviceName);
          return false;
        }

        if (state.isLG && lgIndex == SIZE_MAX)
          lgIndex = displays.size();

        displays.emplace_back(state);
      }

      device = {};
      device.cb = sizeof(device);
    }

    return lgIndex != SIZE_MAX;
  }
}

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

void CPipeClient::ReloadSettings()
{
  if (!m_connected)
    return;

  LGPipeMsg msg;
  msg.size = sizeof(msg);
  msg.type = LGPipeMsg::RELOADSETTINGS;
  WriteMsg(msg);
}

bool CPipeClient::EnsureOnlyDisplayLocked()
{
  std::vector<DisplayState> displays;
  size_t lgIndex;
  if (!GetDisplayStates(displays, lgIndex))
    return false;

  // The only active display is necessarily the primary display.
  if (displays.size() == 1)
    return true;

  for (unsigned int attempt = 0; attempt < 3; ++attempt)
  {
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    LONG result = GetDisplayConfigBufferSizes(
      QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (result != ERROR_SUCCESS)
    {
      DEBUG_ERROR("GetDisplayConfigBufferSizes failed (%ld)", result);
      return false;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,
      &pathCount, paths.data(), &modeCount, modes.data(), NULL);
    if (result == ERROR_INSUFFICIENT_BUFFER)
      continue;

    if (result != ERROR_SUCCESS)
    {
      DEBUG_ERROR("QueryDisplayConfig failed (%ld)", result);
      return false;
    }

    paths.resize(pathCount);
    modes.resize(modeCount);

    for (size_t i = 0; i < paths.size(); ++i)
    {
      DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
      sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
      sourceName.header.size = sizeof(sourceName);
      sourceName.header.adapterId = paths[i].sourceInfo.adapterId;
      sourceName.header.id = paths[i].sourceInfo.id;

      result = DisplayConfigGetDeviceInfo(&sourceName.header);
      if (result != ERROR_SUCCESS)
        continue;

      if (_tcsicmp(sourceName.viewGdiDeviceName,
        displays[lgIndex].device.DeviceName) != 0)
        continue;

      const UINT32 sourceModeIndex = paths[i].sourceInfo.modeInfoIdx;
      const UINT32 targetModeIndex = paths[i].targetInfo.modeInfoIdx;
      if (sourceModeIndex == DISPLAYCONFIG_PATH_MODE_IDX_INVALID ||
        sourceModeIndex >= modes.size() ||
        targetModeIndex == DISPLAYCONFIG_PATH_MODE_IDX_INVALID ||
        targetModeIndex >= modes.size() ||
        modes[sourceModeIndex].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE ||
        modes[targetModeIndex].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
      {
        DEBUG_ERROR("Looking Glass display path has invalid mode indices");
        return false;
      }

      DISPLAYCONFIG_PATH_INFO path = paths[i];
      DISPLAYCONFIG_MODE_INFO selectedModes[2] = {
        modes[sourceModeIndex], modes[targetModeIndex]
      };

      selectedModes[0].sourceMode.position.x = 0;
      selectedModes[0].sourceMode.position.y = 0;
      path.sourceInfo.modeInfoIdx = 0;
      path.targetInfo.modeInfoIdx = 1;
      path.flags |= DISPLAYCONFIG_PATH_ACTIVE;

      // Keep a bootable physical-display topology in the persistence
      // database. Persisting an LG-only topology creates a dependency cycle
      // on Windows 10 at the next boot: IddCx waits for display topology
      // initialization while the saved topology waits for this IDD adapter.
      // The helper reapplies this temporary topology on startup, display
      // changes and periodically, so it remains enforced for the session.
      result = SetDisplayConfig(1, &path, 2, selectedModes,
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
      if (result != ERROR_SUCCESS)
      {
        DEBUG_ERROR("Failed to apply the LG-only display topology (%ld)",
          result);
        return false;
      }

      DEBUG_INFO("Looking Glass display set as the only active display");
      return true;
    }

    DEBUG_ERROR("Looking Glass display configuration path not found");
    return false;
  }

  DEBUG_WARN("Display topology kept changing while selecting Looking Glass");
  return false;
}

bool CPipeClient::EnsureOnlyDisplay()
{
  AcquireSRWLockExclusive(&m_displayLock);
  const bool result = EnsureOnlyDisplayLocked();
  ReleaseSRWLockExclusive(&m_displayLock);
  return result;
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

        case LGPipeMsg::GPUSTATUS:
          HandleGPUStatus(msg);
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
  AcquireSRWLockExclusive(&m_displayLock);

  std::vector<DisplayState> displays;
  size_t lgIndex;
  if (!GetDisplayStates(displays, lgIndex))
  {
    ReleaseSRWLockExclusive(&m_displayLock);
    DEBUG_ERROR("Looking Glass display not found while setting its mode");
    return;
  }

  DEVMODE dm = displays[lgIndex].mode;
  dm.dmPelsWidth        = msg.displayMode.width;
  dm.dmPelsHeight       = msg.displayMode.height;
  dm.dmDisplayFrequency = msg.displayMode.refresh;
  dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;

  LONG result = ChangeDisplaySettingsEx(displays[lgIndex].device.DeviceName,
    &dm, NULL, CDS_UPDATEREGISTRY, NULL);
  if (result != DISP_CHANGE_SUCCESSFUL)
    DEBUG_ERROR("ChangeDisplaySettingsEx Failed (0x%08x)", result);

  ReleaseSRWLockExclusive(&m_displayLock);

  if (result == DISP_CHANGE_SUCCESSFUL)
    EnsureOnlyDisplay();
}

void CPipeClient::HandleGPUStatus(const LGPipeMsg& msg)
{
  CNotifyWindow::instance().setGPU(!msg.gpuStatus.software);
}
