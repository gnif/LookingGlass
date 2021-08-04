/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

#include "interface/platform.h"
#include "common/ivshmem.h"
#include "service.h"
#include "platform.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>

#include <windows.h>
#include <shlwapi.h>
#include <winsvc.h>
#include <psapi.h>
#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>

#define SVCNAME   "Looking Glass (host)"
#define SVC_ERROR ((DWORD)0xC0020001L)
#define LOG_NAME  "looking-glass-host-service.txt"

#define FAIL_MAX_RETRIES         5
#define FAIL_RETRY_INIT_INTERVAL 1000

struct Service
{
  FILE * logFile;
  bool   running;
  HANDLE process;
  HANDLE exitEvent;
  char   exitEventName[64];
};

struct Service service = { 0 };

char logTime[100];

char * currentTime()
{
  time_t t = time(NULL);
  strftime(logTime, sizeof logTime, "%Y-%m-%d %H:%M:%S", localtime(&t));
  return logTime;
}

void doLogReal(const char * fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vfprintf(service.logFile, fmt, args);
  va_end(args);
}

#define doLog(fmt, ...) doLogReal("[%s] " fmt, currentTime(), ##__VA_ARGS__)

static void setupLogging(void)
{
  char logFilePath[MAX_PATH];
  if (!PathCombineA(logFilePath, getSystemLogDirectory(), LOG_NAME))
    strcpy(logFilePath, LOG_NAME);
  service.logFile = fopen(logFilePath, "a+");
  setbuf(service.logFile, NULL);
  doLog("Startup\n");
}

static void finishLogging(void)
{
  doLog("Finished\n");
  fclose(service.logFile);
}

void winerr(void)
{
  char buf[256];
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
    NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
    buf, (sizeof(buf) / sizeof(char)), NULL);
  doLog("0x%08lx - %s", GetLastError(), buf);
}

bool adjustPriv(const char * name, DWORD attributes)
{
  HANDLE           hToken;
  LUID             luid;
  TOKEN_PRIVILEGES tp = { 0 };

  if (!OpenProcessToken(GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
  {
    doLog("failed to open the process\n");
    winerr();
    return false;
  }

  if (!LookupPrivilegeValueA(NULL, name, &luid))
  {
    doLog("failed to lookup the privilege value\n");
    winerr();
    goto fail;
  }

  tp.PrivilegeCount           = 1;
  tp.Privileges[0].Luid       = luid;
  tp.Privileges[0].Attributes = attributes;

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

  CloseHandle(hToken);
  return true;

fail:
  CloseHandle(hToken);
  return false;
}

bool enablePriv(const char * name)
{
  return adjustPriv(name, SE_PRIVILEGE_ENABLED);
}

bool disablePriv(const char * name)
{
  return adjustPriv(name, 0);
}

void Launch(void)
{
  if (service.process)
  {
    CloseHandle(service.process);
    service.process = NULL;
  }

  if (!windowsSetupAPI())
  {
    doLog("windowsSetupAPI failed\n");
    return;
  }

  HANDLE hSystemToken;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE |
        TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_SESSIONID | TOKEN_ADJUST_DEFAULT,
        &hSystemToken))
  {
    doLog("failed to get the system process token\n");
    return;
  }

  HANDLE hToken;
  if (!DuplicateTokenEx(hSystemToken, 0, NULL, SecurityAnonymous,
        TokenPrimary, &hToken))
  {
    doLog("failed to duplicate the system process token\n");
    CloseHandle(hSystemToken);
    return;
  }
  CloseHandle(hSystemToken);

  DWORD origSessionID, targetSessionID, returnedLen;
  GetTokenInformation(hToken, TokenSessionId, &origSessionID,
      sizeof(origSessionID), &returnedLen);

  targetSessionID = WTSGetActiveConsoleSessionId();
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

  if (!enablePriv(SE_INCREASE_QUOTA_NAME))
  {
    doLog("failed to enable " SE_INCREASE_QUOTA_NAME);
    goto fail_token;
  }

  DWORD flags = DETACHED_PROCESS | HIGH_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT;

  PROCESS_INFORMATION pi = {0};
  STARTUPINFO si =
  {
    .cb          = sizeof(STARTUPINFO),
    .dwFlags     = STARTF_USESHOWWINDOW,
    .wShowWindow = SW_SHOW,
    .lpDesktop   = "WinSta0\\Default"
  };

  char * cmdline = NULL;
  char cmdbuf[128];
  if (service.exitEvent)
  {
    snprintf(cmdbuf, sizeof(cmdbuf), "looking-glass-host.exe os:exitEvent=%s",
      service.exitEventName);
    cmdline = cmdbuf;
  }

  if (!f_CreateProcessAsUserA(
      hToken,
      os_getExecutable(),
      cmdline,
      NULL,
      NULL,
      FALSE,
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
    goto fail_token;
  }

  if (!disablePriv(SE_INCREASE_QUOTA_NAME))
    doLog("failed to disable " SE_INCREASE_QUOTA_NAME);

  CloseHandle(pi.hThread);
  service.process = pi.hProcess;
  service.running = true;

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

void Install(void)
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

void Uninstall(void)
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

static bool sleepOrStop(DWORD ms)
{
  switch (WaitForSingleObject(ghSvcStopEvent, ms))
  {
    case WAIT_OBJECT_0:
      return true;

    case WAIT_FAILED:
      doLog("Failed to WaitForSingleObject (0x%lx)\n", GetLastError());
  }
  return false;
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

  /* check if the ivshmem device exists */
  struct IVSHMEM shmDev = { 0 };
  ivshmemOptionsInit();
  if (!ivshmemInit(&shmDev))
  {
    doLog("Unable to find the IVSHMEM device, terminating the service\n");
    goto shutdown;
  }
  ivshmemFree(&shmDev);

  ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);

  UUID uuid;
  RPC_CSTR uuidStr;
  UuidCreate(&uuid);

  if (UuidToString(&uuid, &uuidStr) == RPC_S_OK)
  {
    strcpy(service.exitEventName, "Global\\");
    strcat(service.exitEventName, (const char*) uuidStr);
    RpcStringFree(&uuidStr);

    service.exitEvent = CreateEvent(NULL, FALSE, FALSE, service.exitEventName);
    if (!service.exitEvent)
      doLog("Failed to create exit event: 0x%lx\n", GetLastError());
  }

  int failCount = 0;
  while(1)
  {
    ULONGLONG launchTime = 0ULL;

    DWORD interactiveSession = WTSGetActiveConsoleSessionId();
    if (interactiveSession != 0 && interactiveSession != 0xFFFFFFFF)
    {
      Launch();
      launchTime = GetTickCount64();
    }

    HANDLE waitOn[] = { ghSvcStopEvent, service.process };
    DWORD count = 2;
    DWORD duration = INFINITE;

    if (!service.running)
    {
      // If the service is running, wait only on ghSvcStopEvent and prepare to restart in one second.
      count = 1;
      duration = 1000;
    }

    switch (WaitForMultipleObjects(count, waitOn, FALSE, duration))
    {
      case WAIT_OBJECT_0:
        goto stopped;

      case WAIT_OBJECT_0 + 1:
      {
        service.running = false;

        DWORD code;
        if (!GetExitCodeProcess(service.process, &code))
          doLog("Failed to GetExitCodeProcess (0x%lx)\n", GetLastError());
        else
        {
          doLog("Host application exited with code 0x%lx\n", code);
          switch (code)
          {
            case LG_HOST_EXIT_USER:
              doLog("Host application exited due to user action\n");
              goto stopped;

            case LG_HOST_EXIT_CAPTURE:
              doLog("Host application exited due to capture error; restarting\n");
              failCount = 0;
              break;

            case LG_HOST_EXIT_KILLED:
              doLog("Host application was killed; restarting\n");
              failCount = 0;
              break;

            case LG_HOST_EXIT_FAILED:
            {
              ++failCount;
              if (failCount > FAIL_MAX_RETRIES)
              {
                doLog("Host application failed to start %d times; will not restart\n", FAIL_MAX_RETRIES);
                goto stopped;
              }

              DWORD backoff = FAIL_RETRY_INIT_INTERVAL << (failCount - 1);
              doLog("Host application failed to start %d times, waiting %u ms...\n", failCount, backoff);

              if (sleepOrStop(backoff))
                goto stopped;
              break;
            }

            case LG_HOST_EXIT_FATAL:
              doLog("Host application failed to start with fatal error; will not restart\n");
              goto stopped;

            default:
              doLog("Host application failed due to unknown error; restarting\n");
              break;
          }
        }

        // avoid restarting too often
        if (GetTickCount64() - launchTime < 1000 && sleepOrStop(1000))
          goto stopped;
        break;
      }

      case WAIT_FAILED:
        doLog("Failed to WaitForMultipleObjects (0x%lx)\n", GetLastError());
    }
  }

  stopped:
  if (service.running)
  {
    SetEvent(service.exitEvent);
    switch (WaitForSingleObject(service.process, 1000))
    {
      case WAIT_OBJECT_0:
        service.running = false;
        doLog("Host application exited gracefully\n");
        break;
      case WAIT_TIMEOUT:
        doLog("Host application failed to exit in 1 second\n");
        break;
      case WAIT_FAILED:
        doLog("WaitForSingleObject failed: 0x%lx\n", GetLastError());
        break;
    }

    if (service.running)
    {
      doLog("Terminating the host application\n");
      if (TerminateProcess(service.process, LG_HOST_EXIT_KILLED))
      {
        if (WaitForSingleObject(service.process, INFINITE) == WAIT_OBJECT_0)
          doLog("Host application terminated\n");
        else
          doLog("WaitForSingleObject failed: 0x%lx\n", GetLastError());
      }
      else
        doLog("Failed to terminate the host application\n");
    }

    CloseHandle(service.process);
    service.process = NULL;
  }

shutdown:
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

  return false;
}
