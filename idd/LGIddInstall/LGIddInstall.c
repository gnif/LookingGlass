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
#include <devguid.h>
#include <setupapi.h>
#include <shlwapi.h>
#include <newdev.h>
#include <sddl.h>
#include <aclapi.h>

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
#define DEVICE_COUNT 2

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

bool ensureKeyWithAce()
{
  bool result = false;
  const PCWSTR accountName = L"NT AUTHORITY\\USER MODE DRIVERS";

  HKEY hKey = NULL;
  DWORD disp = 0;
  REGSAM sam = KEY_READ | KEY_WRITE | WRITE_DAC | READ_CONTROL | KEY_WOW64_64KEY;
  PACL oldDacl = NULL;
  PSECURITY_DESCRIPTOR psd = NULL;
  PACL newDacl = NULL;
  PSID pSid = NULL;

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

  pSid = malloc(SECURITY_MAX_SID_SIZE);
  DWORD cbSid = SECURITY_MAX_SID_SIZE;
  if (!CreateWellKnownSid(WinUserModeDriversSid, NULL, pSid, &cbSid))
  {
    debugWinError(L"CreateWellKnownSid", GetLastError());
    goto cleanup;
  }

  EXPLICIT_ACCESSW ea = {0};
  ea.grfAccessPermissions = KEY_ALL_ACCESS;
  ea.grfAccessMode        = GRANT_ACCESS;
  ea.grfInheritance       = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
  ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
  ea.Trustee.ptstrName    = (LPWSTR)pSid;

  ec = SetEntriesInAclW(1, &ea, oldDacl, &newDacl);
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
  if (newDacl) LocalFree(newDacl);
  if (pSid)    free(pSid);
  if (psd)     LocalFree(psd);
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
  LPBOOL pbNeedRestart;
  bool success;
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

enum DeviceCreated {
  DEVICE_CREATED,
  DEVICE_NOT_CREATED,
  DEVICE_UNKNOWN,
};

bool isDeviceCreatedEnum(HDEVINFO hDevInfo, PSP_DEVINFO_DATA pDevInfo, void *pContext)
{
  enum DeviceCreated *result = pContext;
  *result = DEVICE_CREATED;
  return false;
}

enum DeviceCreated isDeviceCreated(const DeviceDesc *device)
{
  enum DeviceCreated result = DEVICE_UNKNOWN;
  if (findDevice(device, isDeviceCreatedEnum, &result) && result == DEVICE_UNKNOWN)
    result = DEVICE_NOT_CREATED;
  return result;
}

bool createDevice(const DeviceDesc *device)
{
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

  if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, hDevInfo, &devInfo))
  {
    debugWinError(L"SetupDiCallClassInstaller", GetLastError());
    goto fail;
  }

  return true;

fail:
  SetupDiDestroyDeviceInfoList(hDevInfo);
  return false;
}

bool destroyDeviceEnum(HDEVINFO hDevInfo, PSP_DEVINFO_DATA pDevInfo, void *pContext)
{
  DeviceRemovalContext *context = pContext;
  BOOL bNeedRestart;
  WCHAR szInfPath[MAX_PATH] = { 0 };

  if (!SetupDiBuildDriverInfoList(hDevInfo, pDevInfo, SPDIT_COMPATDRIVER))
  {
    debugWinError(L"SetupDiBuildDriverInfoList", GetLastError());
    goto uninstall;
  }

  SP_DRVINFO_DATA_W drvInfo = { .cbSize = sizeof drvInfo };
  if (!SetupDiEnumDriverInfoW(hDevInfo, pDevInfo, SPDIT_COMPATDRIVER, 0, &drvInfo))
  {
    debugWinError(L"SetupDiEnumDriverInfoW", GetLastError());
    goto uninstall;
  }

  SP_DRVINFO_DETAIL_DATA_W drvInfoDetail = { .cbSize = sizeof drvInfoDetail };
  SetupDiGetDriverInfoDetailW(hDevInfo, pDevInfo, &drvInfo, &drvInfoDetail, sizeof drvInfoDetail, NULL);

  DWORD dwLastError = GetLastError();
  if (dwLastError == ERROR_INSUFFICIENT_BUFFER)
    wcscpy_s(szInfPath, MAX_PATH, drvInfoDetail.InfFileName);
  else
    debugWinError(L"SetupDiEnumDriverInfoW", GetLastError());

uninstall:
  if (DiUninstallDevice(NULL, hDevInfo, pDevInfo, 0, &bNeedRestart))
    *context->pbNeedRestart |= bNeedRestart;
  else
  {
    debugWinError(L"DiUninstallDevice", GetLastError());
    context->success = false;
    return true;
  }

  if (*szInfPath)
  {
    if (DiUninstallDriverW(NULL, szInfPath, 0, &bNeedRestart))
      *context->pbNeedRestart |= bNeedRestart;
    else
    {
      debugWinError(L"DiUninstallDriverW", GetLastError());
      context->success = false;
    }
  }

  return true;
}

bool destroyDevice(const DeviceDesc *device, LPBOOL pbNeedRestart)
{
  DeviceRemovalContext context = { pbNeedRestart, true };

  return findDevice(device, destroyDeviceEnum, &context) && context.success;
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

void install(const DeviceDesc *const devices[DEVICE_COUNT])
{
  enum DeviceCreated created[DEVICE_COUNT];

  for (size_t i = 0; i < DEVICE_COUNT; ++i)
  {
    created[i] = isDeviceCreated(devices[i]);
    if (created[i] == DEVICE_UNKNOWN)
      exit(1);
  }

  _putws(L"Preparing registry key...");
  if (!ensureKeyWithAce())
    exit(1);

  BOOL bNeedRestart = FALSE;
  BOOL bInfNeedRestart = FALSE;

  for (size_t i = 0; i < DEVICE_COUNT; ++i)
  {
    const DeviceDesc *device = devices[i];

    if (created[i] == DEVICE_NOT_CREATED)
    {
      wprintf(L"Preinstalling %s INF...\n", device->name);
      bInfNeedRestart = FALSE;
      if (!installInf(device, &bInfNeedRestart))
        exit(1);
      bNeedRestart |= bInfNeedRestart;

      wprintf(L"Creating %s device: %s...\n", device->name, device->hardwareId);
      if (!createDevice(device))
        exit(1);
    }

    wprintf(L"Installing %s INF...\n", device->name);
    bInfNeedRestart = FALSE;
    if (!installInf(device, &bInfNeedRestart))
      exit(1);
    bNeedRestart |= bInfNeedRestart;
  }

  if (bNeedRestart)
  {
    _putws(L"Restart required to complete installation");
    exit(12);
  }
}

void uninstall()
{
  BOOL bNeedRestart = 0;
  bool success = true;

  _putws(L"Uninstalling LGInput...");
  success &= destroyDevice(&LGINPUT_DEVICE, &bNeedRestart);

  _putws(L"Uninstalling LGIdd...");
  success &= destroyDevice(&LGIDD_DEVICE, &bNeedRestart);

  DWORD ec = deleteKeyTreeHKLM();
  if (ec != ERROR_SUCCESS)
  {
    debugWinError(L"deleteKeyTreeHKLM failed", ec);
    // this is non-fatal
  }

  if (!success)
    exit(1);

  if (bNeedRestart)
  {
    _putws(L"Restart required to complete installation");
    exit(12);
  }
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
