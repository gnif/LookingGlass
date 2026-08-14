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
#include "CClipboardRing.h"
#include "CDebug.h"
#include "CSRWLock.h"
#include "CNotifyWindow.h"
#include "CRegistrySettings.h"

#include <setupapi.h>
#include <tchar.h>
#include <vector>

namespace
{
  static const unsigned RECOVERY_VERIFY_ATTEMPTS = 20;
  static const unsigned RECOVERY_PATH_ATTEMPTS   = 20;
  static const size_t   RECOVERY_MAX_PATHS       = 4;
  static const DWORD    RECOVERY_VERIFY_DELAY_MS = 100;

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

  bool ContainsNoCase(LPCTSTR text, LPCTSTR value)
  {
    const size_t length = _tcslen(value);
    for (; *text; ++text)
      if (_tcsnicmp(text, value, length) == 0)
        return true;

    return false;
  }

  bool IsLGPath(const DISPLAYCONFIG_PATH_INFO& path)
  {
    DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
    target.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size      = sizeof(target);
    target.header.adapterId = path.targetInfo.adapterId;
    target.header.id        = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS &&
        (_tcsicmp(target.monitorFriendlyDeviceName,
           _T("Looking Glass")) == 0 ||
         ContainsNoCase(target.monitorDevicePath, _T("LGD1DDD")) ||
         ContainsNoCase(target.monitorDevicePath, _T("ROOT#LGIDD"))))
      return true;

    DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
    source.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size      = sizeof(source);
    source.header.adapterId = path.sourceInfo.adapterId;
    source.header.id        = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
        !source.viewGdiDeviceName[0])
      return false;

    DISPLAY_DEVICE device = {};
    device.cb = sizeof(device);
    for (DWORD i = 0; EnumDisplayDevices(NULL, i, &device, 0); ++i)
    {
      if (_tcsicmp(device.DeviceName, source.viewGdiDeviceName) == 0)
        return IsLGDisplay(device);

      device = {};
      device.cb = sizeof(device);
    }

    return false;
  }

  bool SameTarget(const DISPLAYCONFIG_PATH_INFO& a,
    const DISPLAYCONFIG_PATH_INFO& b)
  {
    return a.targetInfo.adapterId.HighPart ==
        b.targetInfo.adapterId.HighPart &&
      a.targetInfo.adapterId.LowPart ==
        b.targetInfo.adapterId.LowPart &&
      a.targetInfo.id == b.targetInfo.id;
  }

  uint32_t QueryAllPaths(std::vector<DISPLAYCONFIG_PATH_INFO>& paths)
  {
    for (unsigned int attempt = 0; attempt < 3; ++attempt)
    {
      UINT32 pathCount = 0;
      UINT32 modeCount = 0;
      LONG result = GetDisplayConfigBufferSizes(
        QDC_ALL_PATHS, &pathCount, &modeCount);
      if (result != ERROR_SUCCESS)
        return static_cast<uint32_t>(result);

      paths.resize(pathCount);
      std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
      result = QueryDisplayConfig(QDC_ALL_PATHS,
        &pathCount, paths.data(), &modeCount, modes.data(), NULL);
      if (result == ERROR_INSUFFICIENT_BUFFER)
        continue;
      if (result != ERROR_SUCCESS)
        return static_cast<uint32_t>(result);

      paths.resize(pathCount);
      return ERROR_SUCCESS;
    }

    return ERROR_INSUFFICIENT_BUFFER;
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
        state.device      = device;
        state.mode.dmSize = sizeof(state.mode);
        state.isLG        = IsLGDisplay(device);

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

  bool HasActiveDisplay(bool lg)
  {
    DISPLAY_DEVICE device = {};
    device.cb = sizeof(device);
    for (DWORD i = 0; EnumDisplayDevices(NULL, i, &device, 0); ++i)
    {
      if ((device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) &&
        !(device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) &&
        IsLGDisplay(device) == lg)
        return true;

      device = {};
      device.cb = sizeof(device);
    }

    return false;
  }

  bool WaitForDisplay(bool lg,
    unsigned int attempts = RECOVERY_VERIFY_ATTEMPTS)
  {
    for (unsigned int attempt = 0;
         attempt < attempts;
         ++attempt)
    {
      if (HasActiveDisplay(lg))
        return true;

      if (attempt + 1 < attempts)
        Sleep(RECOVERY_VERIFY_DELAY_MS);
    }

    return false;
  }

  bool HasOnlyLGDisplay()
  {
    bool found = false;
    DISPLAY_DEVICE device = {};
    device.cb = sizeof(device);
    for (DWORD i = 0; EnumDisplayDevices(NULL, i, &device, 0); ++i)
    {
      if ((device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) &&
          !(device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER))
      {
        if (!IsLGDisplay(device))
          return false;
        found = true;
      }

      device = {};
      device.cb = sizeof(device);
    }

    return found;
  }

  bool WaitForOnlyLGDisplay()
  {
    for (unsigned int attempt = 0;
         attempt < RECOVERY_VERIFY_ATTEMPTS;
         ++attempt)
    {
      if (HasOnlyLGDisplay())
        return true;

      if (attempt + 1 < RECOVERY_VERIFY_ATTEMPTS)
        Sleep(RECOVERY_VERIFY_DELAY_MS);
    }

    return false;
  }

  uint32_t ActivateDisplay(bool lg)
  {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    const uint32_t queryError = QueryAllPaths(paths);
    if (queryError != ERROR_SUCCESS)
    {
      DEBUG_ERROR("Failed to enumerate display paths (%u)", queryError);
      return queryError;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> attempted;
    uint32_t lastError = ERROR_NOT_FOUND;
    for (const DISPLAYCONFIG_PATH_INFO& path : paths)
    {
      if (!path.targetInfo.targetAvailable || IsLGPath(path) != lg)
        continue;

      bool duplicate = false;
      for (const DISPLAYCONFIG_PATH_INFO& previous : attempted)
        if (SameTarget(path, previous))
        {
          duplicate = true;
          break;
        }
      if (duplicate)
        continue;

      if (attempted.size() >= RECOVERY_MAX_PATHS)
        break;
      attempted.emplace_back(path);

      DISPLAYCONFIG_PATH_INFO candidate = path;
      candidate.flags |= DISPLAYCONFIG_PATH_ACTIVE;
      candidate.sourceInfo.modeInfoIdx =
        DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
      candidate.targetInfo.modeInfoIdx =
        DISPLAYCONFIG_PATH_MODE_IDX_INVALID;

      LONG result = SetDisplayConfig(1, &candidate, 0, NULL,
        SDC_APPLY | SDC_TOPOLOGY_SUPPLIED | SDC_ALLOW_CHANGES);
      if (result == ERROR_SUCCESS &&
          WaitForDisplay(lg, RECOVERY_PATH_ATTEMPTS))
      {
        DEBUG_INFO("Activated a saved %s topology",
          lg ? "Looking Glass" : "non-Looking Glass");
        return ERROR_SUCCESS;
      }

      // The connected display may not yet have a database entry. Ask CCD's
      // best-mode logic for a temporary configuration without saving it.
      result = SetDisplayConfig(1, &candidate, 0, NULL,
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
      if (result == ERROR_SUCCESS &&
          WaitForDisplay(lg, RECOVERY_PATH_ATTEMPTS))
      {
        DEBUG_INFO("Activated a fallback %s display",
          lg ? "Looking Glass" : "non-Looking Glass");
        return ERROR_SUCCESS;
      }

      lastError = result == ERROR_SUCCESS ?
        ERROR_NOT_FOUND : static_cast<uint32_t>(result);
    }

    if (attempted.empty())
      DEBUG_ERROR("No connected %s display path was found",
        lg ? "Looking Glass" : "non-Looking Glass");
    else
      DEBUG_ERROR("No %s display path could be activated",
        lg ? "Looking Glass" : "non-Looking Glass");
    return lastError;
  }

  uint32_t ActivateFallbackDisplay()
  {
    return ActivateDisplay(false);
  }

  uint32_t RestoreNonExclusiveTopology()
  {
    if (HasActiveDisplay(true))
      return ERROR_SUCCESS;

    LONG result = SetDisplayConfig(0, NULL, 0, NULL,
      SDC_APPLY | SDC_USE_DATABASE_CURRENT | SDC_ALLOW_CHANGES);
    if (result == ERROR_SUCCESS && WaitForDisplay(true))
    {
      DEBUG_INFO("Saved non-exclusive display topology restored");
      return ERROR_SUCCESS;
    }

    if (result == ERROR_SUCCESS)
      DEBUG_WARN("The saved display topology does not activate Looking Glass");
    else
      DEBUG_WARN("Failed to restore the saved display topology (%ld)", result);

    // If the current database topology omits LG, use Windows' most recently
    // saved clone or extended topology. No persistence flag is supplied, so
    // this does not replace the user's saved display configuration.
    result = SetDisplayConfig(0, NULL, 0, NULL,
      SDC_APPLY | SDC_TOPOLOGY_CLONE | SDC_TOPOLOGY_EXTEND |
      SDC_ALLOW_CHANGES);
    if (result == ERROR_SUCCESS && WaitForDisplay(true))
    {
      DEBUG_INFO("Non-exclusive Looking Glass topology activated");
      return ERROR_SUCCESS;
    }

    return result == ERROR_SUCCESS ?
      ERROR_NOT_FOUND : static_cast<uint32_t>(result);
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

  {
    CSRWExclusiveLock lock(m_clipboardSetupLock);
    if (!PrepareClipboardMappingLocked())
      return false;
  }

  m_endpoint.SetHandler(this);
  return m_endpoint.Start(
    LG_PIPE_NAME,
    CPipeEndpoint::Mode::Client,
    sizeof(LGPipeMsg));
}

void CPipeClient::DeInit()
{
  // Stop first so no endpoint callback can race mapping teardown.
  m_endpoint.Stop();

  CSRWExclusiveLock lock(m_clipboardSetupLock);
  ResetClipboardSetupLocked();
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
  m_endpoint.Send(&msg, sizeof(msg));
}

bool CPipeClient::PipeServerIsAuthorized(HANDLE pipe)
{
  DWORD session = 0xFFFFFFFFU;
  if (!GetNamedPipeServerSessionId(pipe, &session))
  {
    const DWORD error = GetLastError();
    DEBUG_WARN_HR(error, "Failed to identify named pipe server session");
    return false;
  }
  if (session != 0)
  {
    DEBUG_WARN(
      "Rejected named pipe server outside the service session");
    return false;
  }
  return true;
}

bool CPipeClient::BuildPipeClientHello(void * message, size_t size)
{
  CSRWSharedLock setupLock(m_clipboardSetupLock);
  if (!message || size != sizeof(LGPipeMsg) ||
      !m_clipboardMapping || !m_clipboardEpoch)
  {
    DEBUG_ERROR("Clipboard mapping was not prepared before pipe connection");
    return false;
  }

  LGPipeMsg& hello           = *static_cast<LGPipeMsg *>(message);
  hello                      = {};
  hello.size                 = sizeof(hello);
  hello.type                 = LGPipeMsg::HELLO;
  hello.hello.version        = LGPipeMsg::PROTOCOL_VERSION;
  hello.hello.authorityId[0] = m_clipboardAuthorityId[0];
  hello.hello.authorityId[1] = m_clipboardAuthorityId[1];
  return true;
}

void CPipeClient::OnPipeConnected()
{
  bool hasStatus;
  LGPipeMsg status;
  {
    CSRWSharedLock lock(m_displayLock);
    hasStatus = m_hasRecoveryStatus;
    status    = m_recoveryStatus;
  }

  if (hasStatus)
    WriteMsg(status);
}

void CPipeClient::OnPipeDisconnected()
{
  CSRWExclusiveLock lock(m_clipboardSetupLock);
  ResetClipboardSetupLocked();
}

void CPipeClient::ReloadSettings()
{
  if (!m_endpoint.IsConnected())
    return;

  LGPipeMsg msg = {};
  msg.size = sizeof(msg);
  msg.type = LGPipeMsg::RELOADSETTINGS;
  WriteMsg(msg);
}

bool CPipeClient::ShouldReconnect()
{
  const bool attached = IsLGIddDeviceAttached();
  if (!attached)
    DEBUG_INFO("Looking Glass Indirect Display Device was removed");
  else
  {
    CSRWExclusiveLock lock(m_clipboardSetupLock);
    if (!m_clipboardMapping && !PrepareClipboardMappingLocked())
      DEBUG_WARN("Clipboard mapping is not ready for reconnection");
  }
  return attached;
}

bool CPipeClient::EnsureOnlyDisplayLocked()
{
  if (m_recoveryActive)
    return true;

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

uint32_t CPipeClient::RestoreSavedTopologyLocked() const
{
  const LONG result = SetDisplayConfig(0, NULL, 0, NULL,
    SDC_APPLY | SDC_USE_DATABASE_CURRENT | SDC_ALLOW_CHANGES);
  if (result == ERROR_SUCCESS)
  {
    if (WaitForDisplay(false))
    {
      DEBUG_INFO("Recovery display topology activated");
      return ERROR_SUCCESS;
    }

    DEBUG_WARN("The saved topology has no active non-Looking Glass display");
  }
  else
  {
    DEBUG_WARN("Failed to restore the saved display topology (%ld)", result);
  }

  return ActivateFallbackDisplay();
}

uint32_t CPipeClient::RestoreLGTopologyLocked()
{
  uint32_t error = ERROR_SUCCESS;
  bool exclusive = false;

  CRegistrySettings settings;
  const LSTATUS settingsError = settings.open();
  if (settingsError != ERROR_SUCCESS)
  {
    DEBUG_ERROR_HR(settingsError, "Failed to load settings");
    error = static_cast<uint32_t>(settingsError);
  }
  else
  {
    const std::optional<bool> value = settings.getExclusiveMonitor();
    if (!value.has_value())
      error = ERROR_INVALID_DATA;
    else
      exclusive = value.value();
  }

  if (error == ERROR_SUCCESS)
  {
    if (!exclusive)
    {
      error = RestoreNonExclusiveTopology();
      if (error == ERROR_SUCCESS)
      {
        m_recoveryActive = false;
        DEBUG_INFO("Looking Glass display topology restored");
        return ERROR_SUCCESS;
      }
    }
    else
    {
      if (!HasActiveDisplay(true))
        error = ActivateDisplay(true);

      if (error == ERROR_SUCCESS)
      {
        // Keep notification-driven display enforcement suppressed until the
        // LG path is active. This explicit call owns the transition back to
        // the temporary LG-only topology.
        m_recoveryActive = false;
        if (EnsureOnlyDisplayLocked() && WaitForOnlyLGDisplay())
        {
          DEBUG_INFO("Looking Glass display topology restored");
          return ERROR_SUCCESS;
        }

        error = ERROR_GEN_FAILURE;
      }
    }
  }

  // A failed exit must leave a usable recovery display rather than a blank
  // desktop if a partial CCD transition disabled the physical path.
  m_recoveryActive = true;
  if (!HasActiveDisplay(false))
  {
    const uint32_t fallbackError = ActivateFallbackDisplay();
    if (fallbackError != ERROR_SUCCESS)
      DEBUG_ERROR("Failed to restore a recovery display (%u)", fallbackError);
  }
  return error;
}

bool CPipeClient::EnsureOnlyDisplay()
{
  CSRWExclusiveLock lock(m_displayLock);
  return EnsureOnlyDisplayLocked();
}

bool CPipeClient::OnPipeMessage(const void * message, size_t size)
{
  if (size != sizeof(LGPipeMsg))
    return false;

  const LGPipeMsg & msg = *static_cast<const LGPipeMsg *>(message);
  if (msg.size != sizeof(msg))
    return false;

  switch (msg.type)
  {
    case LGPipeMsg::SETCURSORPOS:
      HandleSetCursorPos(msg);
      return true;

    case LGPipeMsg::SETDISPLAYMODE:
      HandleSetDisplayMode(msg);
      return true;

    case LGPipeMsg::GPUSTATUS:
      HandleGPUStatus(msg);
      return true;

    case LGPipeMsg::RESOLUTIONREJECTED:
      HandleResolutionRejected(msg);
      return true;

    case LGPipeMsg::SET_RECOVERY:
      HandleSetRecovery(msg);
      return true;

    case LGPipeMsg::CLIPBOARD_READY:
    {
      CSRWExclusiveLock setupLock(m_clipboardSetupLock);

      if (msg.clipboardReady.status != ERROR_SUCCESS ||
          msg.clipboardReady.epoch != m_clipboardEpoch)
      {
        DEBUG_WARN("IDD rejected the clipboard mapping (%u)",
          msg.clipboardReady.status);
        ResetClipboardSetupLocked();
        return true;
      }

      if (!m_clipboardEnabled)
      {
        const uint64_t epoch = m_clipboardEpoch;
        ResetClipboardSetupLocked();
        setupLock.Unlock();

        LGPipeMsg failure = {};
        failure.size                  = sizeof(failure);
        failure.type                  = LGPipeMsg::CLIPBOARD_READY;
        failure.clipboardReady.epoch  = epoch;
        failure.clipboardReady.status = ERROR_NOT_SUPPORTED;
        m_endpoint.Send(&failure, sizeof(failure));
        return true;
      }

      HANDLE mapping       = nullptr;
      DWORD  failureStatus = ERROR_SUCCESS;
      if (!m_clipboardMapping)
      {
        failureStatus = ERROR_NOT_READY;
        DEBUG_ERROR_HR(failureStatus,
          "Service-owned clipboard mapping is unavailable");
      }
      else if (!DuplicateHandle(GetCurrentProcess(), m_clipboardMapping,
          GetCurrentProcess(), &mapping, 0, FALSE, DUPLICATE_SAME_ACCESS))
      {
        failureStatus = GetLastError();
        DEBUG_ERROR_HR(failureStatus,
          "Failed to duplicate the service-owned clipboard mapping");
      }
      else if (!m_clipboard.Attach(
          mapping, m_clipboardEpoch, true, *this))
      {
        failureStatus = ERROR_INVALID_DATA;
        DEBUG_ERROR_HR(failureStatus,
          "Failed to activate the service-owned clipboard mapping");
      }

      if (failureStatus != ERROR_SUCCESS)
      {
        const uint64_t epoch = m_clipboardEpoch;
        ResetClipboardSetupLocked();
        setupLock.Unlock();

        // Tell the IDD that its successful mapping is unusable here, so it
        // does not leave a one-sided channel advertised as available.
        LGPipeMsg failure = {};
        failure.size                  = sizeof(failure);
        failure.type                  = LGPipeMsg::CLIPBOARD_READY;
        failure.clipboardReady.epoch  = epoch;
        failure.clipboardReady.status = failureStatus;
        m_endpoint.Send(&failure, sizeof(failure));
      }
      return true;
    }

    case LGPipeMsg::CLIPBOARD_KICK:
      m_clipboard.Kick(msg.clipboardKick.epoch);
      return true;

    case LGPipeMsg::CLIPBOARD_RESET:
      m_clipboard.Reset(
        msg.clipboardReset.epoch, msg.clipboardReset.reason);
      return true;

    default:
      DEBUG_ERROR("Unknown message type %d", msg.type);
      return true;
  }
}

bool CPipeClient::ClipboardKick(uint64_t epoch)
{
  LGPipeMsg msg = {};
  msg.size                = sizeof(msg);
  msg.type                = LGPipeMsg::CLIPBOARD_KICK;
  msg.clipboardKick.epoch = epoch;
  return m_endpoint.Send(&msg, sizeof(msg));
}

void CPipeClient::ClipboardResetPeer(uint64_t epoch, uint32_t reason)
{
  LGPipeMsg msg = {};
  msg.size                  = sizeof(msg);
  msg.type                  = LGPipeMsg::CLIPBOARD_RESET;
  msg.clipboardReset.epoch  = epoch;
  msg.clipboardReset.reason = reason;
  m_endpoint.Send(&msg, sizeof(msg));
}

bool CPipeClient::PrepareClipboardMappingLocked()
{
  if (m_clipboardMapping)
    return true;
  if (!m_clipboardMappingId[0] || !m_clipboardMappingId[1])
  {
    DEBUG_ERROR("Invalid service-owned clipboard mapping identifier");
    return false;
  }

  wchar_t mappingName[128];
  const int nameResult = _snwprintf_s(
    mappingName, _countof(mappingName), _TRUNCATE,
    L"Global\\LookingGlassIDDClipboard-%016llx%016llx",
    static_cast<unsigned long long>(m_clipboardMappingId[0]),
    static_cast<unsigned long long>(m_clipboardMappingId[1]));
  if (nameResult < 0)
  {
    DEBUG_ERROR_HR(ERROR_INSUFFICIENT_BUFFER,
      "Failed to format service-owned clipboard mapping name");
    return false;
  }

  m_clipboardMapping = OpenFileMappingW(
    FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mappingName);
  if (!m_clipboardMapping)
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to open service-owned clipboard mapping");
    return false;
  }

  ClipboardMapping * view = static_cast<ClipboardMapping *>(MapViewOfFile(
    m_clipboardMapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
    sizeof(ClipboardMapping)));
  if (!view)
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to initialize service-owned clipboard mapping");
    ResetClipboardSetupLocked();
    return false;
  }

  m_clipboardAuthorityId[0] = view->authorityId[0];
  m_clipboardAuthorityId[1] = view->authorityId[1];
  if (!m_clipboardAuthorityId[0] || !m_clipboardAuthorityId[1])
  {
    DEBUG_ERROR("Invalid clipboard authority identifier");
    UnmapViewOfFile(view);
    ResetClipboardSetupLocked();
    return false;
  }

  ++m_clipboardEpochCounter;
  if (!m_clipboardEpochCounter)
    ++m_clipboardEpochCounter;
  m_clipboardEpoch = m_clipboardEpochCounter;
  CClipboardRing::Initialize(
    *view, m_clipboardEpoch, m_clipboardAuthorityId);
  if (!UnmapViewOfFile(view))
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to unmap initialized service-owned clipboard mapping");
    ResetClipboardSetupLocked();
    return false;
  }
  return true;
}

void CPipeClient::ResetClipboardSetupLocked()
{
  m_clipboard.Detach();
  if (m_clipboardMapping)
    CloseHandle(m_clipboardMapping);
  m_clipboardMapping        = nullptr;
  m_clipboardEpoch          = 0;
  m_clipboardAuthorityId[0] = 0;
  m_clipboardAuthorityId[1] = 0;
}

void CPipeClient::HandleSetCursorPos(const LGPipeMsg& msg)
{
  SetActiveDesktop();
  SetCursorPos(msg.curorPos.x, msg.curorPos.y);
}

void CPipeClient::HandleSetDisplayMode(const LGPipeMsg& msg)
{
  LONG result;
  {
    CSRWExclusiveLock lock(m_displayLock);

    std::vector<DisplayState> displays;
    size_t lgIndex;
    if (!GetDisplayStates(displays, lgIndex))
    {
      DEBUG_ERROR("Looking Glass display not found while setting its mode");
      return;
    }

    DEVMODE dm = displays[lgIndex].mode;
    dm.dmPelsWidth        = msg.displayMode.width;
    dm.dmPelsHeight       = msg.displayMode.height;
    dm.dmDisplayFrequency =
      (msg.displayMode.refreshMilliHz + 500) / 1000;
    dm.dmFields           =
      DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;

    result = ChangeDisplaySettingsEx(displays[lgIndex].device.DeviceName,
      &dm, NULL, CDS_UPDATEREGISTRY, NULL);
    if (result != DISP_CHANGE_SUCCESSFUL)
      DEBUG_ERROR("ChangeDisplaySettingsEx Failed (0x%08x)", result);
  }

  if (result == DISP_CHANGE_SUCCESSFUL)
    EnsureOnlyDisplay();
}

void CPipeClient::HandleGPUStatus(const LGPipeMsg& msg)
{
  CNotifyWindow::instance().setGPU(!msg.gpuStatus.software);
}

void CPipeClient::HandleResolutionRejected(const LGPipeMsg& msg)
{
  CNotifyWindow::instance().notifyResolutionRejected(
    msg.resolutionRejected.width,
    msg.resolutionRejected.height,
    msg.resolutionRejected.requiredSizeMiB);
}

void CPipeClient::HandleSetRecovery(const LGPipeMsg& msg)
{
  const bool active =
    (msg.recovery.request & LGPipeMsg::RECOVERY_ACTIVE) != 0;
  bool      cached = false;
  LGPipeMsg status = {};
  status.size       = sizeof(status);
  status.type       = LGPipeMsg::RECOVERY_FAILED;
  status.recovery   = msg.recovery;

  {
    CSRWExclusiveLock lock(m_displayLock);
    cached = m_hasRecoveryStatus &&
      m_recoveryStatus.recovery.session == msg.recovery.session &&
      m_recoveryStatus.recovery.request == msg.recovery.request;
    if (cached)
      status = m_recoveryStatus;
    else if (active)
    {
      m_recoveryActive = true;
      CNotifyWindow::instance().setRecoveryMode(true);

      const uint32_t error = RestoreSavedTopologyLocked();
      if (error == ERROR_SUCCESS)
        status.type = LGPipeMsg::RECOVERY_ON;
      else if (error == ERROR_NOT_FOUND)
        status.type = LGPipeMsg::RECOVERY_NO_DISPLAY;
      else
        status.type = LGPipeMsg::RECOVERY_FAILED;
    }
    else
    {
      m_recoveryActive = true;
      CNotifyWindow::instance().setRecoveryMode(true);

      if (RestoreLGTopologyLocked() == ERROR_SUCCESS)
      {
        CNotifyWindow::instance().setRecoveryMode(false);
        status.type = LGPipeMsg::RECOVERY_OFF;
      }
    }

    if (!cached)
    {
      m_hasRecoveryStatus = true;
      m_recoveryStatus    = status;
    }
  }

  if (cached)
  {
    DEBUG_TRACE("Replaying cached recovery status");
    WriteMsg(status);
    return;
  }

  if (active)
    DEBUG_INFO("Recovery mode %s", status.type == LGPipeMsg::RECOVERY_ON ?
      "active" : "failed");
  else if (status.type == LGPipeMsg::RECOVERY_OFF)
    DEBUG_INFO("Recovery mode disabled");
  else
    DEBUG_INFO("Recovery mode exit failed");

  WriteMsg(status);
}
