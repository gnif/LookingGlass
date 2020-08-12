/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2020 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#define INSTANCE_MUTEX_NAME "Global\\6f1a5eec-af3f-4a65-99dd-ebe0e4ecea55"

#include "interface/platform.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <inttypes.h>

#include <windows.h>
#include <winsvc.h>
#include <psapi.h>
#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>

#define SVCNAME   "Looking Glass (host)"
#define SVC_ERROR ((DWORD)0xC0020001L)

struct Service
{
  FILE * logFile;
  bool  running;
  DWORD processId;
};

struct Service service = { 0 };

void doLog(const char * fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vfprintf(service.logFile, fmt, args);
  va_end(args);
}

static void setupLogging()
{
  char tempPath[MAX_PATH+1];
  GetTempPathA(sizeof(tempPath), tempPath);
  int len = snprintf(NULL, 0, "%slooking-glass-host-service.txt", tempPath);
  char * logFilePath = malloc(len + 1);
  sprintf(logFilePath, "%slooking-glass-host-service.txt", tempPath);
  service.logFile = fopen(logFilePath, "a+");
  doLog("Startup\n");
}

static void finishLogging()
{
  doLog("Finished\n");
  fclose(service.logFile);
}

void winerr()
{
  char buf[256];
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
    NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
    buf, (sizeof(buf) / sizeof(char)), NULL);
  doLog("0x%08lx - %s", GetLastError(), buf);
}

bool enablePriv(const char * name)
{
  HANDLE           hToken;
  LUID             luid;
  TOKEN_PRIVILEGES tp = { 0 };

  if (!OpenProcessToken(GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
  {
    doLog("failed to open the process\n");
    winerr();
    return -1;
  }

  if (!LookupPrivilegeValueA(NULL, name, &luid))
  {
    doLog("failed to lookup the privilege value\n");
    winerr();
    goto fail;
  }

  tp.PrivilegeCount           = 1;
  tp.Privileges[0].Luid       = luid;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

  if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL,
        NULL))
  {
    doLog("failed to adjust the token privilege\n");
    winerr();
    goto fail;
  }

  if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
  {
    doLog("the token doesn't have the specified privilege - %s\n", name);
    winerr();
    goto fail;
  }

  return true;

fail:
  CloseHandle(hToken);
  return false;
}

HANDLE dupeSystemProcessToken()
{
  DWORD count = 0;
  DWORD returned;
  do
  {
    count += 512;
    DWORD pids[count];
    EnumProcesses(pids, count * sizeof(DWORD), &returned);
  }
  while(returned / sizeof(DWORD) == count);

  DWORD pids[count];
  EnumProcesses(pids, count * sizeof(DWORD), &returned);
  returned /= sizeof(DWORD);

  for(DWORD i = 0; i < returned; ++i)
  {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pids[i]);
    if (!hProcess)
      continue;

    HANDLE hToken;
    if (!OpenProcessToken(hProcess,
          TOKEN_QUERY | TOKEN_READ | TOKEN_IMPERSONATE | TOKEN_QUERY_SOURCE |
          TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_EXECUTE, &hToken))
      goto err_proc;

    DWORD tmp;
    char userBuf[1024];
    TOKEN_USER * user = (TOKEN_USER *)userBuf;
    if (!GetTokenInformation(hToken, TokenUser, user, sizeof(userBuf), &tmp))
      goto err_token;

    CHAR * sid = NULL;
    if (!ConvertSidToStringSidA(user->User.Sid, &sid))
      goto err_token;

    if (strcmp(sid, "S-1-5-18") == 0)
    {
      LocalFree(sid);
      CloseHandle(hProcess);

      // duplicate the token so we can use it
      HANDLE hDupe = NULL;
      if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation,
            TokenPrimary, &hDupe))
        hDupe = NULL;

      CloseHandle(hToken);
      return hDupe;
    }

    LocalFree(sid);
err_token:
    CloseHandle(hToken);
err_proc:
    CloseHandle(hProcess);
  }

  return NULL;
}

DWORD GetInteractiveSessionID()
{
  PWTS_SESSION_INFO pSessionInfo;
  DWORD count;
  DWORD ret = 0;

  if (!WTSEnumerateSessions(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSessionInfo,
        &count))
    return 0;

  for(DWORD i = 0; i < count; ++i)
  {
    if (pSessionInfo[i].State == WTSActive)
    {
      ret = pSessionInfo[i].SessionId;
      break;
    }
  }

  WTSFreeMemory(pSessionInfo);
  return ret;
}

void Launch()
{
  if (!enablePriv(SE_DEBUG_NAME))
    return;

  HANDLE hToken = dupeSystemProcessToken();
  if (!hToken)
  {
    doLog("failed to get the system process token\n");
    return;
  }

  DWORD origSessionID, targetSessionID, returnedLen;
  GetTokenInformation(hToken, TokenSessionId, &origSessionID,
      sizeof(origSessionID), &returnedLen);

  if (!enablePriv(SE_TCB_NAME))
    goto fail_token;

  targetSessionID = GetInteractiveSessionID();
  if (origSessionID != targetSessionID)
  {
    if (!SetTokenInformation(hToken, TokenSessionId,
        &targetSessionID, sizeof(targetSessionID)))
    {
      doLog("failed to set interactive token\n");
      winerr();
      goto fail_token;
    }
  }

  LPVOID pEnvironment = NULL;
  if (!CreateEnvironmentBlock(&pEnvironment, hToken, TRUE))
  {
    doLog("fail_tokened to create the envionment block\n");
    winerr();
    goto fail_token;
  }

  if (!enablePriv(SE_IMPERSONATE_NAME))
    goto fail_token;

  if (!ImpersonateLoggedOnUser(hToken))
  {
    doLog("fail_tokened to impersonate\n");
    winerr();
    goto fail_token;
  }

  if (!enablePriv(SE_ASSIGNPRIMARYTOKEN_NAME))
    goto fail_token;

  if (!enablePriv(SE_INCREASE_QUOTA_NAME))
    goto fail_token;

  DWORD flags = CREATE_NEW_CONSOLE | HIGH_PRIORITY_CLASS;
  if (!pEnvironment)
    flags |= CREATE_UNICODE_ENVIRONMENT;

  PROCESS_INFORMATION pi = {0};
  STARTUPINFO si =
  {
    .cb          = sizeof(STARTUPINFO),
    .dwFlags     = STARTF_USESHOWWINDOW,
    .wShowWindow = SW_SHOW,
    .lpDesktop   = "WinSta0\\Default"
  };

  char * exe = strdup(os_getExecutable());
  if (!CreateProcessAsUserA(
      hToken,
      NULL,
      exe,
      NULL,
      NULL,
      TRUE,
      flags,
      NULL,
      os_getDataPath(),
      &si,
      &pi
    ))
  {
    service.running = false;
    doLog("failed to launch\n");
    winerr();
    goto fail_exe;
  }

  service.processId = pi.dwProcessId;
  service.running   = true;

fail_exe:
  free(exe);

fail_token:
  CloseHandle(hToken);
}

VOID SvcReportEvent(LPTSTR szFunction)
{
  HANDLE hEventSource;
  LPCTSTR lpszStrings[2];
  TCHAR Buffer[80];

  hEventSource = RegisterEventSource(NULL, SVCNAME);

  if (hEventSource)
  {
    snprintf(Buffer, sizeof(Buffer), "%s failed with 0x%lx", szFunction, GetLastError());

    lpszStrings[0] = SVCNAME;
    lpszStrings[1] = Buffer;

    ReportEvent(hEventSource,        // event log handle
                EVENTLOG_ERROR_TYPE, // event type
                0,                   // event category
                SVC_ERROR,           // event identifier
                NULL,                // no security identifier
                2,                   // size of lpszStrings array
                0,                   // no binary data
                lpszStrings,         // array of strings
                NULL);               // no binary data

    DeregisterEventSource(hEventSource);
  }
}

void Install()
{
  TCHAR szPath[MAX_PATH];

  SC_HANDLE schSCManager;
  SC_HANDLE schService;

  if (!GetModuleFileName(NULL, szPath, MAX_PATH))
  {
    doLog("Cannot install service (0x%lx)\n", GetLastError());
    return;
  }

  // Get a handle to the SCM database.

  schSCManager = OpenSCManager(
    NULL,                    // local computer
    NULL,                    // ServicesActive database
    SC_MANAGER_ALL_ACCESS);  // full access rights

  if (NULL == schSCManager)
  {
    doLog("OpenSCManager failed (0x%lx)\n", GetLastError());
    return;
  }

  // Create the service

  schService = CreateService(
    schSCManager,                // SCM database
    SVCNAME,                     // name of service
    SVCNAME,                     // service name to display
    SERVICE_ALL_ACCESS,          // desired access
    SERVICE_WIN32_OWN_PROCESS,   // service type
    SERVICE_AUTO_START,          // start type
    SERVICE_ERROR_NORMAL,        // error control type
    os_getExecutable(),          // path to service's binary
    NULL,                        // no load ordering group
    NULL,                        // no tag identifier
    NULL,                        // no dependencies
    NULL,                        // LocalSystem account
    NULL);                       // no password

  if (schService == NULL)
  {
    doLog("CreateService failed (0x%lx)\n", GetLastError());
    CloseServiceHandle(schSCManager);
    return;
  }
  else
    doLog("Service installed successfully\n");

  // Start the service
  doLog("Starting the service\n");
  StartService(schService, 0, NULL);

  SERVICE_STATUS_PROCESS ssp;
  DWORD dwBytesNeeded;
  if (!QueryServiceStatusEx(schService, SC_STATUS_PROCESS_INFO,
        (LPBYTE)&ssp, sizeof(SERVICE_STATUS_PROCESS), &dwBytesNeeded))
  {
    doLog("QueryServiceStatusEx failed (0x%lx)\n", GetLastError());
    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    return;
  }

  while (ssp.dwCurrentState == SERVICE_START_PENDING)
  {
    DWORD dwWaitTime = ssp.dwWaitHint / 10;
    if(dwWaitTime < 1000)
      dwWaitTime = 1000;
    else if (dwWaitTime > 10000)
      dwWaitTime = 10000;

    Sleep(dwWaitTime);

    if (!QueryServiceStatusEx(schService, SC_STATUS_PROCESS_INFO,
          (LPBYTE)&ssp, sizeof(SERVICE_STATUS_PROCESS), &dwBytesNeeded))
    {
      doLog("QueryServiceStatusEx failed (0x%lx)\n", GetLastError());
      CloseServiceHandle(schService);
      CloseServiceHandle(schSCManager);
      return;
    }
  }

  if (ssp.dwCurrentState != SERVICE_RUNNING)
    doLog("Failed to start the service.\n");
  else
    doLog("Service started.\n");

  CloseServiceHandle(schService);
  CloseServiceHandle(schSCManager);
}

void Uninstall()
{
  SC_HANDLE schSCManager;
  SC_HANDLE schService;

  schSCManager = OpenSCManager(
    NULL,                    // local computer
    NULL,                    // ServicesActive database
    SC_MANAGER_ALL_ACCESS);  // full access rights

  if (NULL == schSCManager)
  {
    doLog("OpenSCManager failed (0x%lx)\n", GetLastError());
    return;
  }

  schService = OpenService(schSCManager, SVCNAME,
      SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);

  if (!schService)
  {
    doLog("OpenService failed (0x%lx)\n", GetLastError());
    CloseServiceHandle(schSCManager);
    return;
  }

  SERVICE_STATUS_PROCESS ssp;
  DWORD dwBytesNeeded;
  if (!QueryServiceStatusEx(schService, SC_STATUS_PROCESS_INFO,
        (LPBYTE)&ssp, sizeof(SERVICE_STATUS_PROCESS), &dwBytesNeeded))
  {
    doLog("QueryServiceStatusEx failed (0x%lx)\n", GetLastError());
    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    return;
  }

  bool stop = false;
  if (ssp.dwCurrentState == SERVICE_RUNNING)
  {
    stop = true;
    doLog("Stopping the service...\n");
    SERVICE_STATUS status;
    if (!ControlService(schService, SERVICE_CONTROL_STOP, &status))
    {
      doLog("ControlService failed (%0xlx)\n", GetLastError());
      CloseServiceHandle(schService);
      CloseServiceHandle(schSCManager);
      return;
    }

    ssp.dwCurrentState = SERVICE_STOP_PENDING;
  }

  while(ssp.dwCurrentState == SERVICE_STOP_PENDING)
  {
    DWORD dwWaitTime = ssp.dwWaitHint / 10;
    if(dwWaitTime < 1000)
      dwWaitTime = 1000;
    else if (dwWaitTime > 10000)
      dwWaitTime = 10000;

    Sleep(dwWaitTime);

    if (!QueryServiceStatusEx(schService, SC_STATUS_PROCESS_INFO,
          (LPBYTE)&ssp, sizeof(SERVICE_STATUS_PROCESS), &dwBytesNeeded))
    {
      doLog("QueryServiceStatusEx failed (0x%lx)\n", GetLastError());
      CloseServiceHandle(schService);
      CloseServiceHandle(schSCManager);
      return;
    }
  }

  if (ssp.dwCurrentState != SERVICE_STOPPED)
  {
      doLog("Failed to stop the service");
      CloseServiceHandle(schService);
      CloseServiceHandle(schSCManager);
      return;
  }

  if (stop)
    doLog("Service stopped.\n");

  if (!DeleteService(schService))
  {
    doLog("DeleteService failed (0x%lx)\n", GetLastError());
    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    return;
  }

  doLog("Service removed.\n");
  CloseServiceHandle(schService);
  CloseServiceHandle(schSCManager);
}

SERVICE_STATUS         gSvcStatus;
SERVICE_STATUS_HANDLE  gSvcStatusHandle;
HANDLE                 ghSvcStopEvent = NULL;

void ReportSvcStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode,
    DWORD dwWaitHint)
{
  static DWORD dwCheckPoint = 1;

  gSvcStatus.dwCurrentState = dwCurrentState;
  gSvcStatus.dwWin32ExitCode = dwWin32ExitCode;
  gSvcStatus.dwWaitHint = dwWaitHint;

  if (dwCurrentState == SERVICE_START_PENDING)
    gSvcStatus.dwControlsAccepted = 0;
  else
    gSvcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;

  if ((dwCurrentState == SERVICE_RUNNING) || (dwCurrentState == SERVICE_STOPPED))
    gSvcStatus.dwCheckPoint = 0;
  else
    gSvcStatus.dwCheckPoint = dwCheckPoint++;

  SetServiceStatus(gSvcStatusHandle, &gSvcStatus);
}

VOID WINAPI SvcCtrlHandler(DWORD dwControl)
{
  switch(dwControl)
  {
    case SERVICE_CONTROL_STOP:
      ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);
      SetEvent(ghSvcStopEvent);
      break;

    case SERVICE_CONTROL_INTERROGATE:
      break;

    default:
      break;
  }

  ReportSvcStatus(gSvcStatus.dwCurrentState, NO_ERROR, 0);
}

VOID WINAPI SvcMain(DWORD dwArgc, LPTSTR *lpszArgv)
{
  gSvcStatusHandle = RegisterServiceCtrlHandler(SVCNAME, SvcCtrlHandler);

  gSvcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  gSvcStatus.dwServiceSpecificExitCode = 0;

  ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 0);

  ghSvcStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
  if (!ghSvcStopEvent)
  {
    ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
    return;
  }

  setupLogging();

  ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);
  while(1)
  {
    /* check if the app is running by trying to take the lock */
    bool running = true;
    HANDLE m = CreateMutex(NULL, FALSE, INSTANCE_MUTEX_NAME);
    if (WaitForSingleObject(m, 0) == WAIT_OBJECT_0)
    {
      running = false;
      service.running = false;
      ReleaseMutex(m);
    }
    CloseHandle(m);

    if (!running && GetInteractiveSessionID() != 0)
    {
      Launch();
      /* avoid being overly agressive in restarting */
      Sleep(1);
    }

    if (WaitForSingleObject(ghSvcStopEvent, 100) == WAIT_OBJECT_0)
      break;
  }

  if (service.running)
  {
    doLog("Terminating the host application\n");
    HANDLE proc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, TRUE,
        service.processId);
    if (proc)
    {
      if (TerminateProcess(proc, 0))
      {
        while(WaitForSingleObject(proc, INFINITE) != WAIT_OBJECT_0) {}
        doLog("Host application terminated\n");
      }
      else
        doLog("Failed to terminate the host application\n");
      CloseHandle(proc);
    }
    else
    {
      doLog("OpenProcess failed (%0xlx)\n", GetLastError());
    }
  }

  ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
  CloseHandle(ghSvcStopEvent);
  finishLogging();
}

bool HandleService(int argc, char * argv[])
{
  service.logFile = stdout;
  if (argc > 1)
  {
    if (strcmp(argv[1], "InstallService") == 0)
    {
      Install();
      return true;
    }

    if (strcmp(argv[1], "UninstallService") == 0)
    {
      Uninstall();
      return true;
    }
  }

  SERVICE_TABLE_ENTRY DispatchTable[] = {
    { SVCNAME, (LPSERVICE_MAIN_FUNCTION) SvcMain },
    { NULL, NULL }
  };

  if (StartServiceCtrlDispatcher(DispatchTable))
    return true;

  /* only allow one instance to run */
  HANDLE m = CreateMutex(NULL, FALSE, INSTANCE_MUTEX_NAME);
  if (WaitForSingleObject(m, 0) != WAIT_OBJECT_0)
  {
    CloseHandle(m);
    return true;
  }

  return false;
}
