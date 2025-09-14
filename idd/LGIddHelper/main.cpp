#include <Windows.h>
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

#define ARRAY_SIZE(x) (sizeof(x) / sizeof*(x))
#define SVCNAME L"Looking Glass (IDD Helper)"

static SERVICE_STATUS_HANDLE l_svcStatusHandle;
static SERVICE_STATUS        l_svcStatus;
static HandleT<EventTraits>  l_svcStopEvent;

bool HandleService();
static void WINAPI SvcMain(DWORD dwArgc, LPTSTR* lpszArgv);
static void WINAPI SvcCtrlHandler(DWORD dwControl);
static void ReportSvcStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint);

static std::wstring              l_executable;
static HandleT<HANDLENullTraits> l_process;

static void Launch();

void CALLBACK DestroyNotifyWindow(PVOID lpParam, BOOLEAN bTimedOut)
{
  DEBUG_INFO("Parent process exited, exiting...");
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

  if (argc != 2)
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

  if (!CNotifyWindow::registerClass())
  {
    DEBUG_ERROR("Failed to register message window class");
    return EXIT_FAILURE;
  }

  if (!g_pipe.Init())
    return EXIT_FAILURE;

  CNotifyWindow window;

  HANDLE hWait;
  if (!RegisterWaitForSingleObject(&hWait, hParent.Get(), DestroyNotifyWindow, &window, INFINITE, WT_EXECUTEONLYONCE))
    DEBUG_ERROR_HR(GetLastError(), "Failed to RegisterWaitForSingleObject");

  MSG msg;
  while (GetMessage(&msg, NULL, 0, 0) > 0)
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  (void) UnregisterWait(hWait);

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

static void WINAPI SvcCtrlHandler(DWORD dwControl)
{
  switch (dwControl)
  {
    case SERVICE_CONTROL_STOP:
      ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);
      SetEvent(l_svcStopEvent.Get());
      return;

    default:
      break;
  }

  ReportSvcStatus(l_svcStatus.dwCurrentState, NO_ERROR, 0);
}

static void WINAPI SvcMain(DWORD dwArgc, LPTSTR* lpszArgv)
{
  l_svcStatus.dwServiceType   = SERVICE_WIN32_OWN_PROCESS;
  l_svcStatus.dwWin32ExitCode = 0;

  l_svcStatusHandle = RegisterServiceCtrlHandler(SVCNAME, SvcCtrlHandler);
  if (!l_svcStatusHandle)
  {
    DEBUG_ERROR_HR(GetLastError(), "RegisterServiceCtrlHandler Failed");
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

  ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);
  bool running = true;
  while (running)
  {
    ULONGLONG launchTime = 0ULL;

    DWORD interactiveSession = WTSGetActiveConsoleSessionId();
    if (interactiveSession != 0 && interactiveSession != 0xFFFFFFFF)
    {
      if (!CPipeClient::IsLGIddDeviceAttached())
      {
        DEBUG_INFO("Looking Glass Indirect Display Device has gone away");
        ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
        return;
      }

      Launch();
      launchTime = GetTickCount64();
    }

    HANDLE waitOn[] = { l_svcStopEvent.Get(), l_process.Get()};
    DWORD count     = 2;
    DWORD duration  = INFINITE;

    if (!l_process.IsValid())
    {
      count    = 1;
      duration = 1000;
    }

    switch (WaitForMultipleObjects(count, waitOn, FALSE, duration))
    {
      // stop requested by the service manager
      case WAIT_OBJECT_0:
        running = false;
        break;

      // child application exited
      case WAIT_OBJECT_0 + 1:
      {
        DWORD code;
        if (!GetExitCodeProcess(l_process.Get(), &code))
        {
          DEBUG_ERROR_HR(GetLastError(), "GetExitCodeProcess Failed");
          break;
        }

        DEBUG_INFO("Child process exited with code 0x%lx", code);
        l_process.Close();
        break;
      }

      case WAIT_FAILED:
        DEBUG_ERROR_HR(GetLastError(), "Failed to WaitForMultipleObjects");
        running = false;
        break;
    }

    if (!running)
      break;

    Sleep(1000);
  }

  ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

static void ReportSvcStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint)
{
  static DWORD dwCheckPoint   = 0;
  l_svcStatus.dwCurrentState  = dwCurrentState;
  l_svcStatus.dwWin32ExitCode = dwWin32ExitCode;
  l_svcStatus.dwWaitHint      = dwWaitHint;

  if (dwCurrentState == SERVICE_START_PENDING)
    l_svcStatus.dwControlsAccepted = 0;
  else
    l_svcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;

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

static void Launch()
{
  l_process.Close();

  HandleT<HANDLENullTraits> sysToken;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE |
    TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_SESSIONID | TOKEN_ADJUST_DEFAULT,
    sysToken.GetAddressOf()))
  {
    DEBUG_ERROR_HR(GetLastError(), "OpenProcessToken failed");
    return;
  }

  HandleT<HANDLENullTraits> token;
  if (!DuplicateTokenEx(sysToken.Get(), 0, NULL, SecurityAnonymous,
    TokenPrimary, token.GetAddressOf()))
  {
    DEBUG_ERROR_HR(GetLastError(), "DuplicateTokenEx failed");
    return;
  }

  DWORD origSessionID, targetSessionID, returnedLen;
  GetTokenInformation(token.Get(), TokenSessionId, &origSessionID,
    sizeof(origSessionID), &returnedLen);

  targetSessionID = WTSGetActiveConsoleSessionId();
  if (origSessionID != targetSessionID)
  {
    if (!SetTokenInformation(token.Get(), TokenSessionId,
      &targetSessionID, sizeof(targetSessionID)))
    {
      DEBUG_ERROR_HR(GetLastError(), "SetTokenInformation failed");
      return;
    }
  }
  
  LPVOID env = NULL;
  if (!CreateEnvironmentBlock(&env, token.Get(), TRUE))
  {
    DEBUG_ERROR_HR(GetLastError(), "CreateEnvironmentBlock failed");
    return;
  }

  if (!EnablePriv(SE_INCREASE_QUOTA_NAME))
  {
    DEBUG_ERROR("Failed to enable %s", SE_INCREASE_QUOTA_NAME);
    return;
  }

  PROCESS_INFORMATION pi = {0};
  STARTUPINFO si = {0};
  si.cb          = sizeof(si);
  si.dwFlags     = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_SHOW;
  si.lpDesktop   = (LPWSTR) L"WinSta0\\Default";

  HandleT<HANDLENullTraits> hProcSync;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
    hProcSync.GetAddressOf(), SYNCHRONIZE, TRUE, 0))
  {
    DEBUG_ERROR("Failed to duplicate own handle for synchronization");
    return;
  }

  wchar_t cmdBuf[128];
  _snwprintf_s(cmdBuf, ARRAY_SIZE(cmdBuf), L"LGIddHelper.exe %" PRId32,
    GetCurrentProcessId());

  if (!CreateProcessAsUser(
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
  ))
  {
    DEBUG_ERROR_HR(GetLastError(), "CreateProcessAsUser failed");
    return;
  }

  DisablePriv(SE_INCREASE_QUOTA_NAME);

  l_process.Attach(pi.hProcess);
  CloseHandle(pi.hThread);
}