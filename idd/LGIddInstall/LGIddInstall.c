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

#define WIN32_LEAN_AND_MEAN

#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <windows.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <setupapi.h>
#include <shlwapi.h>
#include <newdev.h>
#include <sddl.h>
#include <aclapi.h>

#pragma comment(lib, "cfgmgr32.lib")

#define LGIDD_CLASS_GUID GUID_DEVCLASS_DISPLAY
#define LGIDD_CLASS_NAME L"Display"
#define LGIDD_NAME L"LGIdd"
#define LGIDD_HWID L"Root\\LGIdd"
#define LGIDD_HWID_MULTI_SZ (LGIDD_HWID "\0")
#define LGIDD_INF_NAME L"LGIdd.inf"
#define LGIDD_PACKAGE_DIR L"LGIdd"
#define LGINPUT_CLASS_GUID GUID_DEVCLASS_HIDCLASS
#define LGINPUT_CLASS_NAME L"HIDClass"
#define LGINPUT_NAME L"LGInput"
#define LGINPUT_HWID L"Root\\LGInput"
#define LGINPUT_HWID_MULTI_SZ (LGINPUT_HWID "\0")
#define LGINPUT_INF_NAME L"LGInput.inf"
#define LGINPUT_PACKAGE_DIR L"LGInput"
#define LGIDD_REGKEY L"Software\\LookingGlass\\IDD"
#define LGIDD_HELPER_SERVICE L"LGIddHelper"
#define DEVICE_COUNT 2
#define STATE_WAIT_TIMEOUT_MS 30000
#define EXIT_RESTART_REQUIRED 12
// The operation failed, but a partial change or rollback still needs reboot.
#define EXIT_FAILURE_RESTART_REQUIRED 13

typedef struct DeviceDesc
{
  const GUID *classGuid;
  LPCWSTR className;
  LPCWSTR name;
  LPCWSTR hardwareId;
  LPCWSTR hardwareIdMultiSz;
  DWORD hardwareIdMultiSzSize;
  LPCWSTR infName;
  LPCWSTR packageDir;
}
DeviceDesc;

static const DeviceDesc LGIDD_DEVICE =
{
  .classGuid             = &LGIDD_CLASS_GUID,
  .className             = LGIDD_CLASS_NAME,
  .name                  = LGIDD_NAME,
  .hardwareId            = LGIDD_HWID,
  .hardwareIdMultiSz     = LGIDD_HWID_MULTI_SZ,
  .hardwareIdMultiSzSize = sizeof LGIDD_HWID_MULTI_SZ,
  .infName               = LGIDD_INF_NAME,
  .packageDir             = LGIDD_PACKAGE_DIR,
};

static const DeviceDesc LGINPUT_DEVICE =
{
  .classGuid             = &LGINPUT_CLASS_GUID,
  .className             = LGINPUT_CLASS_NAME,
  .name                  = LGINPUT_NAME,
  .hardwareId            = LGINPUT_HWID,
  .hardwareIdMultiSz     = LGINPUT_HWID_MULTI_SZ,
  .hardwareIdMultiSzSize = sizeof LGINPUT_HWID_MULTI_SZ,
  .infName               = LGINPUT_INF_NAME,
  .packageDir             = LGINPUT_PACKAGE_DIR,
};

void usage(wchar_t *program)
{
  wprintf(L"Usage: %s install [LGIdd LGInput|LGInput LGIdd]\n", program);
  wprintf(L"       %s uninstall\n", program);
  exit(2);
}

const DeviceDesc *getDeviceByName(LPCWSTR name)
{
  if (!_wcsicmp(name, LGIDD_DEVICE.name))
    return &LGIDD_DEVICE;

  if (!_wcsicmp(name, LGINPUT_DEVICE.name))
    return &LGINPUT_DEVICE;

  return NULL;
}

void debugWinError(const wchar_t *desc, HRESULT status)
{
  wchar_t *buffer;
  if (!FormatMessageW(
    FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS,
    NULL,
    status,
    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
    (LPWSTR) &buffer,
    1024,
    NULL
  ))
  {
    fwprintf(stderr, L"%s: 0x%08lx: FormatMessage failed with code 0x%08lx\n", desc, status, GetLastError());
    return;
  }

  for (size_t i = wcslen(buffer) - 1; i > 0; --i)
    if (buffer[i] == L'\n' || buffer[i] == L'\r')
      buffer[i] = 0;

  fwprintf(stderr, L"%s: 0x%08lx: %s\n", desc, status, buffer);
  LocalFree(buffer);
}

void debugConfigError(const wchar_t *desc, CONFIGRET status)
{
  debugWinError(desc, CM_MapCrToWin32Err(status, ERROR_GEN_FAILURE));
}

bool queryServiceStatus(
  SC_HANDLE service,
  SERVICE_STATUS_PROCESS *status)
{
  DWORD bytes;
  if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
      (LPBYTE) status, sizeof(*status), &bytes))
  {
    debugWinError(L"QueryServiceStatusEx", GetLastError());
    return false;
  }
  return true;
}

bool waitForServiceState(SC_HANDLE service, DWORD desiredState)
{
  const ULONGLONG deadline = GetTickCount64() + STATE_WAIT_TIMEOUT_MS;
  for (;;)
  {
    SERVICE_STATUS_PROCESS status;
    if (!queryServiceStatus(service, &status))
      return false;
    if (status.dwCurrentState == desiredState)
      return true;

    if (GetTickCount64() >= deadline)
    {
      debugWinError(L"Timed out waiting for LGIddHelper", ERROR_TIMEOUT);
      return false;
    }

    DWORD delay = status.dwWaitHint / 10;
    if (delay < 100)
      delay = 100;
    else if (delay > 1000)
      delay = 1000;
    Sleep(delay);
  }
}

bool stopHelperService(bool *existed, bool *wasRunning)
{
  if (existed)
    *existed = false;
  *wasRunning = false;

  SC_HANDLE manager = OpenSCManagerW(
    NULL, NULL, SC_MANAGER_CONNECT);
  if (!manager)
  {
    debugWinError(L"OpenSCManagerW", GetLastError());
    return false;
  }

  SC_HANDLE service = OpenServiceW(manager, LGIDD_HELPER_SERVICE,
    SERVICE_QUERY_STATUS | SERVICE_STOP);
  if (!service)
  {
    const DWORD error = GetLastError();
    CloseServiceHandle(manager);
    if (error == ERROR_SERVICE_DOES_NOT_EXIST)
      return true;
    debugWinError(L"OpenServiceW(LGIddHelper)", error);
    return false;
  }
  if (existed)
    *existed = true;

  SERVICE_STATUS_PROCESS status;
  bool result = queryServiceStatus(service, &status);
  if (result && status.dwCurrentState != SERVICE_STOPPED)
  {
    *wasRunning = status.dwCurrentState != SERVICE_STOP_PENDING;
    if (status.dwCurrentState == SERVICE_START_PENDING)
      result = waitForServiceState(service, SERVICE_RUNNING);

    if (result && status.dwCurrentState != SERVICE_STOP_PENDING)
    {
      SERVICE_STATUS controlStatus;
      if (!ControlService(service, SERVICE_CONTROL_STOP, &controlStatus))
      {
        const DWORD error = GetLastError();
        if (error != ERROR_SERVICE_NOT_ACTIVE)
        {
          debugWinError(L"ControlService(LGIddHelper, STOP)", error);
          result = false;
        }
      }
    }

    if (result)
      result = waitForServiceState(service, SERVICE_STOPPED);
  }

  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return result;
}

bool deleteHelperService()
{
  SC_HANDLE manager = OpenSCManagerW(
    NULL, NULL, SC_MANAGER_CONNECT);
  if (!manager)
  {
    debugWinError(L"OpenSCManagerW", GetLastError());
    return false;
  }

  SC_HANDLE service = OpenServiceW(manager, LGIDD_HELPER_SERVICE,
    DELETE);
  if (!service)
  {
    const DWORD error = GetLastError();
    CloseServiceHandle(manager);
    if (error == ERROR_SERVICE_DOES_NOT_EXIST)
      return true;
    debugWinError(L"OpenServiceW(LGIddHelper, DELETE)", error);
    return false;
  }

  bool result = true;
  if (!DeleteService(service))
  {
    const DWORD error = GetLastError();
    if (error != ERROR_SERVICE_MARKED_FOR_DELETE)
    {
      debugWinError(L"DeleteService(LGIddHelper)", error);
      result = false;
    }
  }

  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return result;
}

bool startHelperService()
{
  SC_HANDLE manager = OpenSCManagerW(
    NULL, NULL, SC_MANAGER_CONNECT);
  if (!manager)
  {
    debugWinError(L"OpenSCManagerW", GetLastError());
    return false;
  }

  SC_HANDLE service = OpenServiceW(manager, LGIDD_HELPER_SERVICE,
    SERVICE_QUERY_STATUS | SERVICE_START);
  if (!service)
  {
    const DWORD error = GetLastError();
    CloseServiceHandle(manager);
    debugWinError(L"OpenServiceW(LGIddHelper)", error);
    return false;
  }

  bool result = true;
  SERVICE_STATUS_PROCESS status;
  if (!queryServiceStatus(service, &status))
    result = false;
  else if (status.dwCurrentState != SERVICE_RUNNING)
  {
    if (status.dwCurrentState == SERVICE_STOP_PENDING)
      result = waitForServiceState(service, SERVICE_STOPPED);

    if (result && status.dwCurrentState != SERVICE_START_PENDING &&
        !StartServiceW(service, 0, NULL))
    {
      const DWORD error = GetLastError();
      if (error != ERROR_SERVICE_ALREADY_RUNNING)
      {
        debugWinError(L"StartServiceW(LGIddHelper)", error);
        result = false;
      }
    }

    if (result)
      result = waitForServiceState(service, SERVICE_RUNNING);
  }

  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return result;
}

void logRestart(
  const DeviceDesc *device,
  const wchar_t *operation,
  BOOL needRestart)
{
  wprintf(L"%s: %s completed; restart required: %s\n",
    device->name, operation, needRestart ? L"yes" : L"no");
}

bool ensureKeyWithAce()
{
  bool result = false;

  HKEY                 hKey           = NULL;
  DWORD                disp           = 0;
  REGSAM               sam            = KEY_READ | KEY_WRITE | WRITE_DAC |
    READ_CONTROL | KEY_WOW64_64KEY;
  PACL                 oldDacl        = NULL;
  PSECURITY_DESCRIPTOR psd            = NULL;
  PACL                 newDacl        = NULL;
  PSID                 driverSid      = NULL;
  PSID                 interactiveSid = NULL;

  DWORD ec = RegCreateKeyExW(HKEY_LOCAL_MACHINE, LGIDD_REGKEY, 0, NULL, 0, sam, NULL, &hKey, &disp);
  if (ec != ERROR_SUCCESS)
  {
    debugWinError(L"RegCreateKeyExW", ec);
    return false;
  }

  ec = GetSecurityInfo(hKey, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, NULL, NULL, &oldDacl, NULL, &psd);
  if (ec != ERROR_SUCCESS)
  {
    debugWinError(L"GetSecurityInfo", ec);
    goto cleanup;
  }

  driverSid = malloc(SECURITY_MAX_SID_SIZE);
  if (!driverSid)
  {
    debugWinError(L"malloc(USER MODE DRIVERS SID)", ERROR_OUTOFMEMORY);
    goto cleanup;
  }
  DWORD cbSid = SECURITY_MAX_SID_SIZE;
  if (!CreateWellKnownSid(
      WinUserModeDriversSid, NULL, driverSid, &cbSid))
  {
    debugWinError(L"CreateWellKnownSid(WinUserModeDriversSid)",
      GetLastError());
    goto cleanup;
  }

  interactiveSid = malloc(SECURITY_MAX_SID_SIZE);
  if (!interactiveSid)
  {
    debugWinError(L"malloc(INTERACTIVE SID)", ERROR_OUTOFMEMORY);
    goto cleanup;
  }
  cbSid = SECURITY_MAX_SID_SIZE;
  if (!CreateWellKnownSid(
      WinInteractiveSid, NULL, interactiveSid, &cbSid))
  {
    debugWinError(L"CreateWellKnownSid(WinInteractiveSid)",
      GetLastError());
    goto cleanup;
  }

  EXPLICIT_ACCESSW ea[2] = {0};
  ea[0].grfAccessPermissions = KEY_QUERY_VALUE | KEY_SET_VALUE;
  ea[0].grfAccessMode        = SET_ACCESS;
  ea[0].grfInheritance       = NO_INHERITANCE;
  ea[0].Trustee.TrusteeForm  = TRUSTEE_IS_SID;
  ea[0].Trustee.ptstrName    = (LPWSTR)driverSid;

  // The interactive Helper may update values on this exact key, but cannot
  // create subkeys, delete it, or change its security descriptor.
  ea[1].grfAccessPermissions = KEY_QUERY_VALUE | KEY_SET_VALUE;
  ea[1].grfAccessMode        = SET_ACCESS;
  ea[1].grfInheritance       = NO_INHERITANCE;
  ea[1].Trustee.TrusteeForm  = TRUSTEE_IS_SID;
  ea[1].Trustee.ptstrName    = (LPWSTR)interactiveSid;

  ec = SetEntriesInAclW(ARRAYSIZE(ea), ea, oldDacl, &newDacl);
  if (ec != ERROR_SUCCESS)
  {
    debugWinError(L"SetEntriesInAclW", ec);
    goto cleanup;
  }

  ec = SetSecurityInfo(hKey, SE_REGISTRY_KEY,
    DACL_SECURITY_INFORMATION,
    NULL, NULL, newDacl, NULL);

  if (ec != ERROR_SUCCESS)
  {
    debugWinError(L"SetSecurityInfo", ec);
    goto cleanup;
  }

  result = true;

  cleanup:
  if (newDacl)
    LocalFree(newDacl);
  if (interactiveSid)
    free(interactiveSid);
  if (driverSid)
    free(driverSid);
  if (psd)
    LocalFree(psd);
  RegCloseKey(hKey);

  return result;
}

DWORD deleteKeyTreeHKLM()
{
  HKEY h;
  DWORD ec = RegOpenKeyExW(HKEY_LOCAL_MACHINE, LGIDD_REGKEY, 0, KEY_WRITE | KEY_WOW64_64KEY, &h);
  if (ec != ERROR_SUCCESS)
    return ec;

  ec = RegDeleteTreeW(h, NULL);
  RegCloseKey(h);
  return ec;
}

typedef bool (*DEVICE_FOUND_PROC)(HDEVINFO hDevInfo, PSP_DEVINFO_DATA pDevInfo, void *pContext);

typedef struct DeviceRemovalContext
{
  const DeviceDesc *device;
  LPBOOL pbNeedRestart;
  bool success;
  bool deviceRemoved;
}
DeviceRemovalContext;

bool findDevice(const DeviceDesc *device, DEVICE_FOUND_PROC procFound, void *pContext)
{
  HDEVINFO hDevInfo = SetupDiGetClassDevsW(device->classGuid, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
  if (hDevInfo == INVALID_HANDLE_VALUE)
  {
    debugWinError(L"SetupDiGetClassDevsW", GetLastError());
    return false;
  }

  SP_DEVINFO_DATA devInfo = { .cbSize = sizeof devInfo, 0 };
  for (DWORD dwIndex = 0; SetupDiEnumDeviceInfo(hDevInfo, dwIndex, &devInfo); ++dwIndex)
  {
    DWORD dwSizeRequired;
    DWORD dwPropertyType;
    SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfo, SPDRP_HARDWAREID, &dwPropertyType, NULL, 0, &dwSizeRequired);

    DWORD dwLastError = GetLastError();
    if (dwLastError == ERROR_INVALID_DATA)
      continue;
    else if (dwLastError != ERROR_INSUFFICIENT_BUFFER)
    {
      debugWinError(L"SetupDiGetDeviceRegistryPropertyW(SPDRP_HARDWAREID) size calculation", dwLastError);
      goto fail;
    }

    if (dwPropertyType != REG_MULTI_SZ)
    {
      fwprintf(stderr, L"SetupDiGetDeviceRegistryPropertyW(SPDRP_HARDWAREID) returned wrong type\n");
      goto fail;
    }

    LPWSTR lpBuffer = malloc(dwSizeRequired);
    if (!lpBuffer)
    {
      fwprintf(stderr, L"failed to allocate memory for SetupDiGetDeviceRegistryPropertyW(SPDRP_HARDWAREID)\n");
      goto fail;
    }

    if (!SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfo, SPDRP_HARDWAREID, &dwPropertyType, (PBYTE)lpBuffer, dwSizeRequired, NULL))
    {
      debugWinError(L"SetupDiGetDeviceRegistryPropertyW(SPDRP_HARDWAREID) for real", GetLastError());
      free(lpBuffer);
      goto fail;
    }

    bool found = false;

    for (LPWSTR lpHwId = lpBuffer; *lpHwId; lpHwId += wcslen(lpBuffer) + 1)
    {
      if (!lstrcmpiW(lpHwId, device->hardwareId))
      {
        found = true;
        break;
      }
    }

    free(lpBuffer);

    if (found && !procFound(hDevInfo, &devInfo, pContext))
      break;
  }

  SetupDiDestroyDeviceInfoList(hDevInfo);
  return true;

fail:
  SetupDiDestroyDeviceInfoList(hDevInfo);
  return false;
}

typedef struct DeviceRuntimeState
{
  const DeviceDesc *device;
  bool success;
  bool present;
  bool started;
  bool usable;
  bool created;
  bool rollbackDeviceSafe;
  WCHAR instanceId[MAX_DEVICE_ID_LEN];
  WCHAR stagedInfPath[MAX_PATH];
}
DeviceRuntimeState;

bool captureDeviceStateEnum(
  HDEVINFO hDevInfo,
  PSP_DEVINFO_DATA pDevInfo,
  void *pContext)
{
  DeviceRuntimeState *state = pContext;
  if (!SetupDiGetDeviceInstanceIdW(hDevInfo, pDevInfo,
      state->instanceId, ARRAYSIZE(state->instanceId), NULL))
  {
    debugWinError(L"SetupDiGetDeviceInstanceIdW", GetLastError());
    state->success = false;
    return false;
  }

  ULONG status;
  ULONG problem;
  const CONFIGRET cr = CM_Get_DevNode_Status(
    &status, &problem, pDevInfo->DevInst, 0);
  if (cr != CR_SUCCESS)
  {
    debugConfigError(L"CM_Get_DevNode_Status", cr);
    state->success = false;
    return false;
  }

  state->present = true;
  state->started = (status & DN_STARTED) != 0;
  state->usable  = state->started &&
    !(status & (DN_HAS_PROBLEM | DN_WILL_BE_REMOVED));
  return false;
}

bool captureDeviceState(
  const DeviceDesc *device,
  DeviceRuntimeState *state)
{
  ZeroMemory(state, sizeof(*state));
  state->device  = device;
  state->success = true;
  return findDevice(device, captureDeviceStateEnum, state) &&
    state->success;
}

bool deviceIsUsable(const DeviceDesc *device)
{
  DeviceRuntimeState state;
  return captureDeviceState(device, &state) &&
    state.present && state.usable;
}

DeviceRuntimeState *runtimeStateForDevice(
  const DeviceDesc *device,
  DeviceRuntimeState *idd,
  DeviceRuntimeState *input)
{
  return device == &LGIDD_DEVICE ? idd : input;
}

bool createDevice(DeviceRuntimeState *state)
{
  const DeviceDesc *device = state->device;
  HDEVINFO hDevInfo = SetupDiCreateDeviceInfoList(device->classGuid, NULL);
  if (hDevInfo == INVALID_HANDLE_VALUE)
  {
    debugWinError(L"SetupDiCreateDeviceInfoList", GetLastError());
    return false;
  }

  SP_DEVINFO_DATA devInfo = { .cbSize = sizeof devInfo, 0 };
  if (!SetupDiCreateDeviceInfoW(hDevInfo, device->className, device->classGuid, NULL, NULL, DICD_GENERATE_ID, &devInfo))
  {
    debugWinError(L"SetupDiCreateDeviceInfoW", GetLastError());
    goto fail;
  }

  if (!SetupDiSetDeviceRegistryPropertyW(hDevInfo, &devInfo, SPDRP_HARDWAREID,
    (PBYTE) device->hardwareIdMultiSz, device->hardwareIdMultiSzSize))
  {
    debugWinError(L"SetupDiSetDeviceRegistryPropertyW", GetLastError());
    goto fail;
  }

  if (!SetupDiGetDeviceInstanceIdW(hDevInfo, &devInfo,
      state->instanceId, ARRAYSIZE(state->instanceId), NULL))
  {
    debugWinError(L"SetupDiGetDeviceInstanceIdW", GetLastError());
    goto fail;
  }

  if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, hDevInfo, &devInfo))
  {
    debugWinError(L"SetupDiCallClassInstaller", GetLastError());
    goto fail;
  }

  SetupDiDestroyDeviceInfoList(hDevInfo);
  return true;

fail:
  SetupDiDestroyDeviceInfoList(hDevInfo);
  return false;
}

bool destroyDeviceEnum(HDEVINFO hDevInfo, PSP_DEVINFO_DATA pDevInfo, void *pContext)
{
  DeviceRemovalContext *context = pContext;
  BOOL bNeedRestart = FALSE;
  WCHAR szInfPath[MAX_PATH] = { 0 };

  SP_DEVINSTALL_PARAMS_W installParams =
    { .cbSize = sizeof installParams };
  if (!SetupDiGetDeviceInstallParamsW(hDevInfo, pDevInfo,
      &installParams))
  {
    debugWinError(L"SetupDiGetDeviceInstallParamsW", GetLastError());
    context->success = false;
    goto uninstall;
  }

  installParams.FlagsEx |= DI_FLAGSEX_INSTALLEDDRIVER;
  if (!SetupDiSetDeviceInstallParamsW(hDevInfo, pDevInfo,
      &installParams))
  {
    debugWinError(L"SetupDiSetDeviceInstallParamsW", GetLastError());
    context->success = false;
    goto uninstall;
  }

  if (!SetupDiBuildDriverInfoList(hDevInfo, pDevInfo, SPDIT_COMPATDRIVER))
  {
    debugWinError(L"SetupDiBuildDriverInfoList", GetLastError());
    context->success = false;
    goto uninstall;
  }

  SP_DRVINFO_DATA_W drvInfo = { .cbSize = sizeof drvInfo };
  if (!SetupDiEnumDriverInfoW(hDevInfo, pDevInfo,
      SPDIT_COMPATDRIVER, 0, &drvInfo))
  {
    const DWORD error = GetLastError();
    if (error == ERROR_NO_MORE_ITEMS)
      goto uninstall;
    debugWinError(L"SetupDiEnumDriverInfoW", error);
    context->success = false;
    goto uninstall;
  }

  SP_DRVINFO_DETAIL_DATA_W drvInfoDetail =
    { .cbSize = sizeof drvInfoDetail };
  if (SetupDiGetDriverInfoDetailW(hDevInfo, pDevInfo, &drvInfo,
      &drvInfoDetail, sizeof drvInfoDetail, NULL) ||
      GetLastError() == ERROR_INSUFFICIENT_BUFFER)
  {
    wcscpy_s(szInfPath, ARRAYSIZE(szInfPath),
      drvInfoDetail.InfFileName);
  }
  else
  {
    debugWinError(L"SetupDiGetDriverInfoDetailW", GetLastError());
    context->success = false;
  }

uninstall:
  if (DiUninstallDevice(NULL, hDevInfo, pDevInfo, 0, &bNeedRestart))
  {
    logRestart(context->device, L"DiUninstallDevice", bNeedRestart);
    *context->pbNeedRestart |= bNeedRestart;
    context->deviceRemoved = true;
  }
  else
  {
    debugWinError(L"DiUninstallDevice", GetLastError());
    context->success = false;
    return true;
  }

  if (*szInfPath)
  {
    bNeedRestart = FALSE;
    if (DiUninstallDriverW(NULL, szInfPath, 0, &bNeedRestart))
    {
      logRestart(context->device, L"DiUninstallDriverW", bNeedRestart);
      *context->pbNeedRestart |= bNeedRestart;
    }
    else
    {
      debugWinError(L"DiUninstallDriverW", GetLastError());
      context->success = false;
    }
  }

  return true;
}

bool destroyDevice(
  const DeviceDesc *device,
  LPBOOL pbNeedRestart,
  bool *deviceRemoved)
{
  DeviceRemovalContext context =
  {
    .device        = device,
    .pbNeedRestart = pbNeedRestart,
    .success       = true,
  };

  const bool found = findDevice(device, destroyDeviceEnum, &context);
  if (deviceRemoved)
    *deviceRemoved = context.deviceRemoved;

  return found && context.success;
}

bool getInfPath(const DeviceDesc *device, LPWSTR lpszInf)
{
  WCHAR szDir[MAX_PATH];
  WCHAR szPackageDir[MAX_PATH];

  if (!GetModuleFileNameW(NULL, szDir, MAX_PATH))
  {
    debugWinError(L"GetModuleFileNameW", GetLastError());
    return false;
  }

  *PathFindFileNameW(szDir) = 0;
  if (!PathCombineW(lpszInf, szDir, device->infName))
  {
    debugWinError(L"PathCombineW", GetLastError());
    return false;
  }

  if (!PathFileExistsW(lpszInf))
  {
    if (!PathCombineW(szPackageDir, szDir, device->packageDir) ||
        !PathCombineW(lpszInf, szPackageDir, device->infName))
    {
      debugWinError(L"PathCombineW", GetLastError());
      return false;
    }

    if (!PathFileExistsW(lpszInf))
    {
      fwprintf(stderr, L"INF file does not exist: %s\n", lpszInf);
      return false;
    }
  }

  return true;
}

bool stageInf(DeviceRuntimeState *state)
{
  WCHAR sourceInf[MAX_PATH];
  state->stagedInfPath[0] = 0;

  if (!getInfPath(state->device, sourceInf))
    return false;

  // NOOVERWRITE distinguishes packages introduced by this transaction
  // from packages that must survive a failed fresh installation.
  if (SetupCopyOEMInfW(sourceInf, NULL, SPOST_PATH,
      SP_COPY_NOOVERWRITE, state->stagedInfPath,
      ARRAYSIZE(state->stagedInfPath), NULL, NULL))
  {
    wprintf(L"%s: staged new package as %s\n",
      state->device->name, state->stagedInfPath);
    return true;
  }

  const DWORD error = GetLastError();
  if (error == ERROR_FILE_EXISTS)
  {
    if (*state->stagedInfPath)
      wprintf(L"%s: package already staged as %s\n",
        state->device->name, state->stagedInfPath);
    else
      wprintf(L"%s: package is already staged\n",
        state->device->name);
    state->stagedInfPath[0] = 0;
    return true;
  }

  state->stagedInfPath[0] = 0;
  debugWinError(L"SetupCopyOEMInfW", error);
  return false;
}

bool installInf(const DeviceDesc *device, PBOOL pbNeedRestart)
{
  WCHAR szInf[MAX_PATH];

  if (!getInfPath(device, szInf))
    return false;

  if (!DiInstallDriverW(NULL, szInf, DIIRFLAG_FORCE_INF, pbNeedRestart))
  {
    debugWinError(L"DiInstallDriverW", GetLastError());
    return false;
  }

  return true;
}

bool removeCreatedDevice(
  const DeviceRuntimeState *state,
  LPBOOL pbNeedRestart)
{
  HDEVINFO hDevInfo = SetupDiCreateDeviceInfoList(
    state->device->classGuid, NULL);
  if (hDevInfo == INVALID_HANDLE_VALUE)
  {
    debugWinError(L"SetupDiCreateDeviceInfoList", GetLastError());
    return false;
  }

  SP_DEVINFO_DATA devInfo = { .cbSize = sizeof devInfo };
  if (!SetupDiOpenDeviceInfoW(hDevInfo, state->instanceId,
      NULL, 0, &devInfo))
  {
    const DWORD error = GetLastError();
    SetupDiDestroyDeviceInfoList(hDevInfo);
    if (error == ERROR_NO_SUCH_DEVINST)
      return true;
    debugWinError(L"SetupDiOpenDeviceInfoW", error);
    return false;
  }

  BOOL callNeedRestart = FALSE;
  if (!DiUninstallDevice(NULL, hDevInfo, &devInfo, 0,
      &callNeedRestart))
  {
    const DWORD error = GetLastError();
    SetupDiDestroyDeviceInfoList(hDevInfo);
    debugWinError(L"DiUninstallDevice (rollback)", error);
    return false;
  }

  SetupDiDestroyDeviceInfoList(hDevInfo);
  logRestart(state->device, L"DiUninstallDevice (rollback)",
    callNeedRestart);
  *pbNeedRestart |= callNeedRestart;
  return true;
}

bool rollbackCreatedDevice(
  DeviceRuntimeState *state,
  LPBOOL pbNeedRestart)
{
  if (!state->created)
    return true;

  wprintf(L"Rolling back %s device...\n", state->device->name);
  if (!removeCreatedDevice(state, pbNeedRestart))
    return false;

  state->rollbackDeviceSafe = true;
  state->present = false;
  return true;
}

bool rollbackStagedPackage(
  DeviceRuntimeState *state,
  LPBOOL pbNeedRestart)
{
  if (!*state->stagedInfPath)
    return true;

  if (state->created && !state->rollbackDeviceSafe)
  {
    fwprintf(stderr,
      L"%s: retaining newly staged package because device rollback "
      L"failed\n", state->device->name);
    return false;
  }

  BOOL callNeedRestart = FALSE;
  wprintf(L"Rolling back %s package...\n", state->device->name);
  if (!DiUninstallDriverW(NULL, state->stagedInfPath, 0,
      &callNeedRestart))
  {
    debugWinError(L"DiUninstallDriverW (rollback)", GetLastError());
    return false;
  }

  logRestart(state->device, L"DiUninstallDriverW (rollback)",
    callNeedRestart);
  *pbNeedRestart |= callNeedRestart;
  state->stagedInfPath[0] = 0;
  return true;
}

bool rollbackInstall(
  DeviceRuntimeState *idd,
  DeviceRuntimeState *input,
  LPBOOL pbNeedRestart)
{
  bool success = true;

  const bool inputDeviceSafe =
    rollbackCreatedDevice(input, pbNeedRestart);
  if (!inputDeviceSafe)
    success = false;

  if (inputDeviceSafe)
  {
    if (!rollbackCreatedDevice(idd, pbNeedRestart))
      success = false;
  }
  else if (idd->created)
  {
    fwprintf(stderr,
      L"LGIdd: retaining newly created device because LGInput "
      L"rollback failed\n");
  }

  bool packageOrderSafe = inputDeviceSafe;
  if (packageOrderSafe)
  {
    if (!rollbackStagedPackage(input, pbNeedRestart))
    {
      packageOrderSafe = false;
      success = false;
    }
  }
  else if (*input->stagedInfPath)
  {
    fwprintf(stderr,
      L"LGInput: retaining newly staged package because device "
      L"rollback failed\n");
  }

  if (packageOrderSafe)
  {
    if (!rollbackStagedPackage(idd, pbNeedRestart))
      success = false;
  }
  else if (*idd->stagedInfPath)
  {
    fwprintf(stderr,
      L"LGIdd: retaining newly staged package because LGInput package "
      L"rollback failed\n");
    success = false;
  }

  return success;
}

void install(const DeviceDesc *const devices[DEVICE_COUNT])
{
  DeviceRuntimeState idd;
  DeviceRuntimeState input;
  bool helperExisted = false;
  bool helperWasRunning = false;
  bool installSucceeded = false;
  bool rollbackSucceeded = true;
  bool restoreSucceeded = true;
  BOOL needRestart = FALSE;

  if (!captureDeviceState(&LGIDD_DEVICE, &idd) ||
      !captureDeviceState(&LGINPUT_DEVICE, &input))
    exit(1);

  _putws(L"Stopping LGIddHelper...");
  if (!stopHelperService(&helperExisted, &helperWasRunning))
    exit(1);

  _putws(L"Preparing registry key...");
  if (!ensureKeyWithAce())
    goto cleanup;

  for (size_t i = 0; i < DEVICE_COUNT; ++i)
  {
    const DeviceDesc *device = devices[i];
    DeviceRuntimeState *state = runtimeStateForDevice(
      device, &idd, &input);
    BOOL callNeedRestart = FALSE;

    if (!state->present)
    {
      wprintf(L"Staging %s INF...\n", device->name);
      if (!stageInf(state))
        goto cleanup;

      wprintf(L"Creating %s device: %s...\n", device->name, device->hardwareId);
      if (!createDevice(state))
        goto cleanup;
      state->present = true;
      state->created = true;
    }

    wprintf(L"Installing %s INF...\n", device->name);
    callNeedRestart = FALSE;
    if (!installInf(device, &callNeedRestart))
      goto cleanup;
    logRestart(device, L"DiInstallDriverW", callNeedRestart);
    needRestart |= callNeedRestart;
  }

  installSucceeded = true;

cleanup:
  if (!installSucceeded)
  {
    rollbackSucceeded = rollbackInstall(&idd, &input, &needRestart);
    if (!helperExisted && idd.created && idd.rollbackDeviceSafe)
    {
      _putws(L"Removing LGIddHelper created by failed installation...");
      if (!deleteHelperService())
        rollbackSucceeded = false;
    }
  }

  if (installSucceeded && restoreSucceeded && !needRestart)
  {
    _putws(L"Starting LGIddHelper...");
    if (!startHelperService())
      restoreSucceeded = false;
  }
  else if (!installSucceeded && restoreSucceeded && helperWasRunning)
  {
    _putws(L"Restoring LGIddHelper after failed installation...");
    if (!startHelperService())
      restoreSucceeded = false;
  }

  if (!installSucceeded)
  {
    if (!rollbackSucceeded)
      fwprintf(stderr, L"Installation rollback was incomplete\n");
    if (needRestart)
    {
      _putws(L"Restart required after failed installation");
      exit(EXIT_FAILURE_RESTART_REQUIRED);
    }
    exit(1);
  }

  if (needRestart)
  {
    _putws(L"Restart required to complete installation");
    exit(EXIT_RESTART_REQUIRED);
  }

  if (!restoreSucceeded)
    exit(1);
}

void uninstall()
{
  BOOL bNeedRestart = FALSE;
  bool helperWasRunning;
  bool iddRemoved = false;

  _putws(L"Stopping LGIddHelper...");
  if (!stopHelperService(NULL, &helperWasRunning))
    exit(1);

  _putws(L"Uninstalling LGInput...");
  if (!destroyDevice(&LGINPUT_DEVICE, &bNeedRestart, NULL))
    goto failure;

  _putws(L"Uninstalling LGIdd...");
  if (!destroyDevice(&LGIDD_DEVICE, &bNeedRestart, &iddRemoved))
    goto failure;

  DWORD ec = deleteKeyTreeHKLM();
  if (ec != ERROR_SUCCESS)
  {
    debugWinError(L"deleteKeyTreeHKLM failed", ec);
    // this is non-fatal
  }

  if (bNeedRestart)
  {
    _putws(L"Restart required to complete uninstallation");
    exit(EXIT_RESTART_REQUIRED);
  }
  return;

failure:
  if (helperWasRunning && !iddRemoved && deviceIsUsable(&LGIDD_DEVICE))
  {
    _putws(L"Restoring LGIddHelper after failed uninstallation...");
    if (!startHelperService())
      fwprintf(stderr,
        L"LGIddHelper could not be restored after uninstall failure\n");
  }
  if (bNeedRestart)
  {
    _putws(L"Restart required after failed uninstallation");
    exit(EXIT_FAILURE_RESTART_REQUIRED);
  }
  exit(1);
}

int wmain(int argc, wchar_t **argv)
{
  _setmode(_fileno(stderr), _O_U16TEXT);

  if (argc < 2)
    usage(argv[0]);

  if (!wcscmp(argv[1], L"install"))
  {
    const DeviceDesc *devices[DEVICE_COUNT] = { &LGIDD_DEVICE, &LGINPUT_DEVICE };

    if (argc != 2)
    {
      if (argc != 4)
        usage(argv[0]);

      devices[0] = getDeviceByName(argv[2]);
      devices[1] = getDeviceByName(argv[3]);
      if (!devices[0] || !devices[1] || devices[0] == devices[1])
        usage(argv[0]);
    }

    install(devices);
  }
  else if (!wcscmp(argv[1], L"uninstall"))
  {
    if (argc != 2)
      usage(argv[0]);
    uninstall();
  }
  else
    usage(argv[0]);
}
