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

#include <Windows.h>
#include <Ole2.h>
#include <wrl.h>
#include <UserEnv.h>

#include <cinttypes>
#include <vector>
#include <string>

using namespace Microsoft::WRL::Wrappers;
using namespace Microsoft::WRL::Wrappers::HandleTraits;

#include "CDebug.h"
#include "VersionInfo.h"
#include "CPipeClient.h"
#include "CNotifyWindow.h"
#include "CConfigWindow.h"

#include "common/array.h"

#define SVCNAME L"Looking Glass (IDD Helper)"

static constexpr DWORD NO_CONSOLE_SESSION = 0xFFFFFFFFu;

static SERVICE_STATUS_HANDLE l_svcStatusHandle;
static SERVICE_STATUS        l_svcStatus;
static HandleT<EventTraits>  l_svcStopEvent;
static HandleT<EventTraits>  l_svcSessionChangeEvent;

bool HandleService();
static void WINAPI SvcMain(DWORD dwArgc, LPTSTR* lpszArgv);
static DWORD WINAPI SvcCtrlHandler(DWORD dwControl, DWORD dwEventType,
  LPVOID lpEventData, LPVOID lpContext);
static void ReportSvcStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint);

static std::wstring              l_executable;
static HandleT<HANDLENullTraits> l_process;
static HandleT<EventTraits>      l_childStopEvent;
static DWORD                     l_desiredSession = NO_CONSOLE_SESSION;
static DWORD                     l_childSession   = NO_CONSOLE_SESSION;

struct OleScope
{
  ~OleScope() { OleUninitialize(); }
};

static bool Launch(DWORD sessionId);
static bool StopChild();

void CALLBACK DestroyNotifyWindow(PVOID lpParam, BOOLEAN bTimedOut)
{
  (void) bTimedOut;
  DEBUG_INFO("Helper shutdown requested, exiting...");
  CNotifyWindow *window = (CNotifyWindow *)lpParam;
  window->close();
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd)
{
  wchar_t buffer[MAX_PATH];
  DWORD result = GetModuleFileName(NULL, buffer, MAX_PATH);
  if (result == 0)
  {
    DEBUG_ERROR("Failed to get the executable path");
    return EXIT_FAILURE;
  }  
  l_executable = buffer;

  int argc = 0;
  LPWSTR * wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> args;
  args.reserve(argc);
  for (int i = 0; i < argc; ++i)
    args.emplace_back(wargv[i]);
  LocalFree(wargv);

  if (argc == 1)
  {
    g_debug.Init(L"looking-glass-idd-service");
    DEBUG_INFO("Looking Glass IDD Helper Service (" LG_VERSION_STR ")");
    if (!HandleService())
      return EXIT_FAILURE;
    return EXIT_SUCCESS;
  }

  if (argc != 2 && argc != 3)
    return EXIT_FAILURE;

  // child process
  g_debug.Init(L"looking-glass-idd-helper");
  DEBUG_INFO("Looking Glass IDD Helper Process (" LG_VERSION_STR ")");

  HandleT<HANDLENullTraits> hParent(OpenProcess(SYNCHRONIZE, FALSE, std::stoul(args[1])));
  if (!hParent.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to open parent process");
    return EXIT_FAILURE;
  }

  HandleT<EventTraits> hStop;
  if (argc == 3)
  {
    hStop.Attach(OpenEvent(SYNCHRONIZE, FALSE, args[2].c_str()));
    if (!hStop.IsValid())
    {
      DEBUG_ERROR_HR(GetLastError(), "Failed to open the child stop event");
      return EXIT_FAILURE;
    }
  }

  const HRESULT ole = OleInitialize(nullptr);
  if (FAILED(ole))
  {
    DEBUG_ERROR_HR(ole, "Failed to initialize OLE");
    return EXIT_FAILURE;
  }
  const OleScope oleScope;

  const HRESULT security = CoInitializeSecurity(nullptr, 0, nullptr, nullptr,
    RPC_C_AUTHN_LEVEL_NONE, RPC_C_IMP_LEVEL_IDENTIFY, nullptr, EOAC_NONE,
    nullptr);
  if (FAILED(security))
  {
    DEBUG_ERROR_HR(security, "Failed to initialize COM security");
    return EXIT_FAILURE;
  }

  if (!CNotifyWindow::registerClass())
  {
    DEBUG_ERROR("Failed to register message window class");
    return EXIT_FAILURE;
  }

  if (!CConfigWindow::registerClass())
  {
    DEBUG_ERROR("Failed to register config window class");
    return EXIT_FAILURE;
  }

  CNotifyWindow& window = CNotifyWindow::instance();

  if (!window.initClipboard(g_pipe.Clipboard()))
    DEBUG_ERROR("Failed to initialize clipboard synchronization");
  else
    g_pipe.EnableClipboard();

  // the pipe must be initialized after the CNotifyWindow
  // has been created to avoid a potential race
  if (!g_pipe.Init())
    return EXIT_FAILURE;

  window.onSettingChange([]() {
    g_pipe.ReloadSettings();
  });

  window.onEnsureOnlyDisplay([]() {
    return g_pipe.EnsureOnlyDisplay();
  });

  HANDLE hParentWait = NULL;
  if (!RegisterWaitForSingleObject(&hParentWait, hParent.Get(),
      DestroyNotifyWindow, &window, INFINITE, WT_EXECUTEONLYONCE))
    DEBUG_ERROR_HR(GetLastError(), "Failed to RegisterWaitForSingleObject");

  HANDLE hStopWait = NULL;
  if (hStop.IsValid() &&
      !RegisterWaitForSingleObject(&hStopWait, hStop.Get(),
        DestroyNotifyWindow, &window, INFINITE, WT_EXECUTEONLYONCE))
    DEBUG_ERROR_HR(GetLastError(), "Failed to register the child stop wait");

  MSG msg;
  while (GetMessage(&msg, NULL, 0, 0) > 0)
  {
    HWND hDlg = window.hwndDialog();
    if (!hDlg || !IsDialogMessage(hDlg, &msg))
    {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  }

  if (hParentWait)
    (void) UnregisterWaitEx(hParentWait, INVALID_HANDLE_VALUE);
  if (hStopWait)
    (void) UnregisterWaitEx(hStopWait, INVALID_HANDLE_VALUE);

  DEBUG_INFO("Helper window destroyed.");
  g_pipe.DeInit();
  return EXIT_SUCCESS;
}

bool HandleService()
{  
  SERVICE_TABLE_ENTRY DispatchTable[] =
  {
    { (LPWSTR) SVCNAME, SvcMain },
    { NULL, NULL }
  };

  if (StartServiceCtrlDispatcher(DispatchTable) == FALSE)
  {
    DEBUG_ERROR_HR(GetLastError(), "StartServiceCtrlDispatcher Failed");
    return false;
  }

  return true;
}

static DWORD WINAPI SvcCtrlHandler(DWORD dwControl, DWORD dwEventType,
  LPVOID lpEventData, LPVOID lpContext)
{
  (void) dwEventType;
  (void) lpEventData;
  (void) lpContext;

  switch (dwControl)
  {
    case SERVICE_CONTROL_STOP:
      ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
      SetEvent(l_svcStopEvent.Get());
      return NO_ERROR;

    case SERVICE_CONTROL_SESSIONCHANGE:
      if (l_svcSessionChangeEvent.IsValid())
        SetEvent(l_svcSessionChangeEvent.Get());
      return NO_ERROR;

    case SERVICE_CONTROL_INTERROGATE:
      ReportSvcStatus(l_svcStatus.dwCurrentState, NO_ERROR, 0);
      return NO_ERROR;

    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

static void WINAPI SvcMain(DWORD dwArgc, LPTSTR* lpszArgv)
{
  l_svcStatus.dwServiceType   = SERVICE_WIN32_OWN_PROCESS;
  l_svcStatus.dwWin32ExitCode = 0;
  l_desiredSession = NO_CONSOLE_SESSION;
  l_childSession   = NO_CONSOLE_SESSION;

  l_svcStatusHandle = RegisterServiceCtrlHandlerExW(SVCNAME,
    SvcCtrlHandler, NULL);
  if (!l_svcStatusHandle)
  {
    DEBUG_ERROR_HR(GetLastError(), "RegisterServiceCtrlHandlerExW Failed");
    return;
  }

  if (!CPipeClient::IsLGIddDeviceAttached())
  {
    DEBUG_INFO("Looking Glass Indirect Display Device not found, not starting.");
    ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
    return;
  }

  ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 0);
  l_svcStopEvent.Attach(CreateEvent(NULL, TRUE, FALSE, NULL));
  if (!l_svcStopEvent.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "CreateEvent Failed");
    ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
    return;
  }

  l_svcSessionChangeEvent.Attach(CreateEvent(NULL, FALSE, FALSE, NULL));
  if (!l_svcSessionChangeEvent.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "CreateEvent Failed");
    ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
    return;
  }

  ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);
  bool running = true;
  ULONGLONG nextLaunch = 0;
  while (running)
  {
    if (WaitForSingleObject(l_svcStopEvent.Get(), 0) == WAIT_OBJECT_0)
      break;

    DWORD interactiveSession = WTSGetActiveConsoleSessionId();
    if (l_desiredSession != interactiveSession)
    {
      if (interactiveSession == NO_CONSOLE_SESSION)
        DEBUG_INFO("No active console session");
      else
        DEBUG_INFO("Active console session changed to %lu", interactiveSession);

      l_desiredSession = interactiveSession;
      nextLaunch = 0;
    }

    if (l_process.IsValid() && l_childSession != l_desiredSession)
    {
      if (!StopChild())
      {
        running = false;
        break;
      }

      // Re-evaluate both the stop event and the active console session before
      // launching a replacement child.
      continue;
    }

    if (!l_process.IsValid() &&
        l_desiredSession != NO_CONSOLE_SESSION &&
        GetTickCount64() >= nextLaunch)
    {
      if (!CPipeClient::IsLGIddDeviceAttached())
      {
        DEBUG_INFO("Looking Glass Indirect Display Device has gone away");
        running = false;
        break;
      }

      if (!Launch(l_desiredSession))
        nextLaunch = GetTickCount64() + 1000;
    }

    HANDLE waitOn[] =
    {
      l_svcStopEvent.Get(),
      l_svcSessionChangeEvent.Get(),
      l_process.Get()
    };
    DWORD count     = 3;
    DWORD duration  = INFINITE;

    if (!l_process.IsValid())
    {
      count    = 2;
      duration = 1000;
    }

    switch (WaitForMultipleObjects(count, waitOn, FALSE, duration))
    {
      // stop requested by the service manager
      case WAIT_OBJECT_0:
        running = false;
        break;

      // active console session may have changed
      case WAIT_OBJECT_0 + 1:
        break;

      // child application exited
      case WAIT_OBJECT_0 + 2:
      {
        DWORD code;
        if (!GetExitCodeProcess(l_process.Get(), &code))
          DEBUG_ERROR_HR(GetLastError(), "GetExitCodeProcess Failed");
        else
          DEBUG_INFO("Child process exited with code 0x%lx", code);

        l_process.Close();
        l_childStopEvent.Close();
        l_childSession = NO_CONSOLE_SESSION;
        nextLaunch = GetTickCount64() + 1000;
        break;
      }

      case WAIT_FAILED:
        DEBUG_ERROR_HR(GetLastError(), "Failed to WaitForMultipleObjects");
        running = false;
        break;
    }
  }

  (void) StopChild();
  ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

static void ReportSvcStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint)
{
  static DWORD dwCheckPoint   = 0;
  l_svcStatus.dwCurrentState  = dwCurrentState;
  l_svcStatus.dwWin32ExitCode = dwWin32ExitCode;
  l_svcStatus.dwWaitHint      = dwWaitHint;

  if (dwCurrentState == SERVICE_RUNNING)
    l_svcStatus.dwControlsAccepted =
      SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SESSIONCHANGE;
  else
    l_svcStatus.dwControlsAccepted = 0;

  if ((dwCurrentState == SERVICE_RUNNING) || (dwCurrentState == SERVICE_STOPPED))
    l_svcStatus.dwCheckPoint = 0;
  else
    l_svcStatus.dwCheckPoint = ++dwCheckPoint;

  SetServiceStatus(l_svcStatusHandle, &l_svcStatus);
}

//static void 

static bool EnablePriv(LPCWSTR name)
{
  TOKEN_PRIVILEGES tp = { 0 };
  LUID luid;
  HandleT<HANDLENullTraits> hToken;

  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
    hToken.GetAddressOf()))
  {
    DEBUG_ERROR_HR(GetLastError(), "OpenProcessToken");
    return false;
  }

  if (!LookupPrivilegeValue(NULL, name, &luid))
  {
    DEBUG_ERROR_HR(GetLastError(), "LookupPrivilegeValue %s", name);
    return false;
  }

  tp.PrivilegeCount = 1;
  tp.Privileges[0].Luid = luid;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

  if (!AdjustTokenPrivileges(hToken.Get(), FALSE, &tp, sizeof(tp), NULL, NULL))
  {
    DEBUG_ERROR_HR(GetLastError(), "AdjustTokenPrivileges %s", name);
    return false;
  }

  return true;
}

static void DisablePriv(LPCWSTR name)
{
  TOKEN_PRIVILEGES tp = {0};
  LUID luid;
  HandleT<HANDLENullTraits> hToken;

  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
    hToken.GetAddressOf()))
  {
    DEBUG_ERROR_HR(GetLastError(), "OpenProcessToken");
    return;
  }

  if (!LookupPrivilegeValue(NULL, name, &luid))
  {
    DEBUG_ERROR_HR(GetLastError(), "LookupPrivilegeValue %s", name);
    return;
  }

  tp.PrivilegeCount = 1;
  tp.Privileges[0].Luid = luid;
  tp.Privileges[0].Attributes = 0;

  if (!AdjustTokenPrivileges(hToken.Get(), FALSE, &tp, sizeof(tp), NULL, NULL))
    DEBUG_ERROR_HR(GetLastError(), "AdjustTokenPrivileges %s", name);
}

static bool Launch(DWORD sessionId)
{
  if (l_process.IsValid())
    return false;

  HandleT<HANDLENullTraits> sysToken;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE |
    TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_SESSIONID | TOKEN_ADJUST_DEFAULT,
    sysToken.GetAddressOf()))
  {
    DEBUG_ERROR_HR(GetLastError(), "OpenProcessToken failed");
    return false;
  }

  HandleT<HANDLENullTraits> token;
  if (!DuplicateTokenEx(sysToken.Get(), 0, NULL, SecurityAnonymous,
    TokenPrimary, token.GetAddressOf()))
  {
    DEBUG_ERROR_HR(GetLastError(), "DuplicateTokenEx failed");
    return false;
  }

  DWORD origSessionID, returnedLen;
  if (!GetTokenInformation(token.Get(), TokenSessionId, &origSessionID,
      sizeof(origSessionID), &returnedLen))
  {
    DEBUG_ERROR_HR(GetLastError(), "GetTokenInformation failed");
    return false;
  }

  if (origSessionID != sessionId)
  {
    if (!SetTokenInformation(token.Get(), TokenSessionId,
      &sessionId, sizeof(sessionId)))
    {
      DEBUG_ERROR_HR(GetLastError(), "SetTokenInformation failed");
      return false;
    }
  }
  
  LPVOID env = NULL;
  if (!CreateEnvironmentBlock(&env, token.Get(), TRUE))
  {
    DEBUG_ERROR_HR(GetLastError(), "CreateEnvironmentBlock failed");
    return false;
  }

  if (!EnablePriv(SE_INCREASE_QUOTA_NAME))
  {
    DEBUG_ERROR("Failed to enable %s", SE_INCREASE_QUOTA_NAME);
    DestroyEnvironmentBlock(env);
    return false;
  }

  PROCESS_INFORMATION pi = {0};
  STARTUPINFO si = {0};
  si.cb          = sizeof(si);
  si.dwFlags     = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_SHOW;
  si.lpDesktop   = (LPWSTR) L"WinSta0\\Default";

  wchar_t stopEventName[128];
  _snwprintf_s(stopEventName, ARRAY_LENGTH(stopEventName), _TRUNCATE,
    L"Global\\LookingGlassIDDHelperStop-%lu-%lu-%" PRIu64,
    GetCurrentProcessId(), sessionId, GetTickCount64());

  l_childStopEvent.Attach(CreateEvent(NULL, TRUE, FALSE, stopEventName));
  if (!l_childStopEvent.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create the child stop event");
    DisablePriv(SE_INCREASE_QUOTA_NAME);
    DestroyEnvironmentBlock(env);
    return false;
  }

  wchar_t cmdBuf[256];
  _snwprintf_s(cmdBuf, ARRAY_LENGTH(cmdBuf), _TRUNCATE,
    L"LGIddHelper.exe %" PRIu32 L" %s", GetCurrentProcessId(), stopEventName);

  const bool created = CreateProcessAsUser(
    token.Get(),
    l_executable.c_str(),
    cmdBuf,
    NULL,
    NULL,
    FALSE,
    DETACHED_PROCESS | HIGH_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT,
    env,
    NULL,
    &si,
    &pi
  );
  const DWORD createError = created ? ERROR_SUCCESS : GetLastError();

  DisablePriv(SE_INCREASE_QUOTA_NAME);
  DestroyEnvironmentBlock(env);

  if (!created)
  {
    DEBUG_ERROR_HR(createError, "CreateProcessAsUser failed");
    l_childStopEvent.Close();
    return false;
  }

  l_process.Attach(pi.hProcess);
  CloseHandle(pi.hThread);
  l_childSession = sessionId;
  DEBUG_INFO("Started child process %lu in session %lu",
    pi.dwProcessId, sessionId);
  return true;
}

static bool StopChild()
{
  if (!l_process.IsValid())
  {
    l_childStopEvent.Close();
    l_childSession = NO_CONSOLE_SESSION;
    return true;
  }

  DEBUG_INFO("Stopping child process in session %lu", l_childSession);
  if (l_childStopEvent.IsValid() && !SetEvent(l_childStopEvent.Get()))
    DEBUG_ERROR_HR(GetLastError(), "Failed to signal the child stop event");

  DWORD result = WaitForSingleObject(l_process.Get(), 5000);
  if (result == WAIT_TIMEOUT)
  {
    DEBUG_WARN("Child process did not stop in time, terminating it");
    if (!TerminateProcess(l_process.Get(), EXIT_FAILURE))
    {
      DEBUG_ERROR_HR(GetLastError(), "Failed to terminate child process");
      return false;
    }
    else
      result = WaitForSingleObject(l_process.Get(), 1000);
  }
  else if (result == WAIT_FAILED)
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to wait for child process");
    return false;
  }

  if (result != WAIT_OBJECT_0)
  {
    DEBUG_ERROR("Child process did not terminate");
    return false;
  }

  l_process.Close();
  l_childStopEvent.Close();
  l_childSession = NO_CONSOLE_SESSION;
  return true;
}
