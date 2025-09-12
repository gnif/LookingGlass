#define WIN32_LEAN_AND_MEAN

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <windows.h>
#include <devguid.h>
#include <setupapi.h>
#include <shlwapi.h>
#include <newdev.h>

#define LGIDD_CLASS_GUID GUID_DEVCLASS_DISPLAY
#define LGIDD_CLASS_NAME L"Display"
#define LGIDD_HWID L"Root\\LGIdd"
#define LGIDD_HWID_MULTI_SZ (LGIDD_HWID "\0")
#define LGIDD_INF_NAME L"LGIdd.inf"

void usage(wchar_t *program)
{
  wprintf(L"Usage: %s <install|uninstall>\n", program);
  exit(2);
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

typedef bool (*IDD_FOUND_PROC)(HDEVINFO hDevInfo, PSP_DEVINFO_DATA pDevInfo, void *pContext);

bool findIddDevice(IDD_FOUND_PROC procFound, void *pContext)
{
  HDEVINFO hDevInfo = SetupDiGetClassDevsW(&LGIDD_CLASS_GUID, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
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
      if (!lstrcmpiW(lpHwId, LGIDD_HWID))
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

bool isIddDeviceCreatedEnum(HDEVINFO hDevInfo, PSP_DEVINFO_DATA pDevInfo, void *pContext)
{
  enum DeviceCreated *result = pContext;
  *result = DEVICE_CREATED;
  return false;
}

enum DeviceCreated isIddDeviceCreated()
{
  enum DeviceCreated result = DEVICE_UNKNOWN;
  if (findIddDevice(isIddDeviceCreatedEnum, &result) && result == DEVICE_UNKNOWN)
    result = DEVICE_NOT_CREATED;
  return result;
}

bool createIddDevice(void)
{
  HDEVINFO hDevInfo = SetupDiCreateDeviceInfoList(&LGIDD_CLASS_GUID, NULL);
  if (hDevInfo == INVALID_HANDLE_VALUE)
  {
    debugWinError(L"SetupDiCreateDeviceInfoList", GetLastError());
    return false;
  }

  SP_DEVINFO_DATA devInfo = { .cbSize = sizeof devInfo, 0 };
  if (!SetupDiCreateDeviceInfoW(hDevInfo, LGIDD_CLASS_NAME, &LGIDD_CLASS_GUID, NULL, NULL, DICD_GENERATE_ID, &devInfo))
  {
    debugWinError(L"SetupDiCreateDeviceInfoW", GetLastError());
    goto fail;
  }

  if (!SetupDiSetDeviceRegistryPropertyW(hDevInfo, &devInfo, SPDRP_HARDWAREID, (PBYTE) LGIDD_HWID_MULTI_SZ, sizeof LGIDD_HWID_MULTI_SZ))
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

bool destroyIddDeviceEnum(HDEVINFO hDevInfo, PSP_DEVINFO_DATA pDevInfo, void* pContext)
{
  LPBOOL pbNeedRestart = pContext;
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
    *pbNeedRestart |= bNeedRestart;
  else
  {
    debugWinError(L"DiUninstallDevice", GetLastError());
    return true;
  }

  if (*szInfPath)
  {
    if (DiUninstallDriverW(NULL, szInfPath, 0, &bNeedRestart))
      *pbNeedRestart |= bNeedRestart;
    else
      debugWinError(L"DiUninstallDriverW", GetLastError());
  }

  return true;
}

void destroyIddDevice(LPBOOL pbNeedRestart)
{
  findIddDevice(destroyIddDeviceEnum, pbNeedRestart);
}

bool getIddInfPath(LPWSTR lpszInf)
{
  WCHAR szDir[MAX_PATH];
  WCHAR szInf[MAX_PATH];

  if (!GetModuleFileNameW(NULL, szDir, MAX_PATH))
  {
    debugWinError(L"GetModuleFileNameW", GetLastError());
    return false;
  }

  *PathFindFileNameW(szDir) = 0;
  if (!PathCombineW(lpszInf, szDir, LGIDD_INF_NAME))
  {
    debugWinError(L"PathCombineW", GetLastError());
    return false;
  }

  if (!PathFileExistsW(lpszInf))
  {
    fwprintf(stderr, L"INF file does not exist: %s\n", szInf);
    return false;
  }

  return true;
}

bool installIddInf(PBOOL pbNeedRestart)
{
  WCHAR szInf[MAX_PATH];

  if (!getIddInfPath(szInf))
    return false;

  if (!DiInstallDriverW(NULL, szInf, DIIRFLAG_FORCE_INF, pbNeedRestart))
  {
    debugWinError(L"DiInstallDriverW", GetLastError());
    return false;
  }

  return true;
}

void install()
{
  switch (isIddDeviceCreated())
  {
  case DEVICE_NOT_CREATED:
    wprintf(L"Creating LGIdd device: %s...\n", LGIDD_HWID);
    if (!createIddDevice())
      return;

    // fallthrough
  case DEVICE_CREATED:
    _putws(L"Installing INF...");
    BOOL bNeedRestart;
    if (!installIddInf(&bNeedRestart))
      return;

    if (bNeedRestart)
    {
      _putws(L"Restart required to complete installation");
      exit(12);
    }
    break;
  case DEVICE_UNKNOWN:
    exit(1);
  }
}

void uninstall()
{
  BOOL bNeedRestart = 0;
  destroyIddDevice(&bNeedRestart);

  if (bNeedRestart)
  {
    _putws(L"Restart required to complete installation");
    exit(12);
  }
}

int wmain(int argc, wchar_t **argv)
{
  if (argc != 2)
    usage(argv[0]);

  if (!wcscmp(argv[1], L"install"))
    install();
  else if (!wcscmp(argv[1], L"uninstall"))
    uninstall();
  else
    usage(argv[0]);
}
