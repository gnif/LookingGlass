#include <Windows.h>
#include <wrl.h>
#include <UserEnv.h>

#include <vector>
#include <string>

using namespace Microsoft::WRL::Wrappers;
using namespace Microsoft::WRL::Wrappers::HandleTraits;

#include "CDebug.h"
#include "VersionInfo.h"
#include "CPipeClient.h"

#define SVCNAME "Looking Glass (IDD Helper)"

static SERVICE_STATUS_HANDLE l_svcStatusHandle;
static SERVICE_STATUS        l_svcStatus;
static HandleT<EventTraits>  l_svcStopEvent;

bool HandleService();
static void WINAPI SvcMain(DWORD dwArgc, LPTSTR* lpszArgv);
static void WINAPI SvcCtrlHandler(DWORD dwControl);
static void ReportSvcStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint);

static std::string               l_executable;
static HandleT<HANDLENullTraits> l_process;
static HandleT<EventTraits>      l_exitEvent;
static std::string               l_exitEventName;

static void Launch();

LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd)
{
  g_debug.Init("looking-glass-iddhelper");
  DEBUG_INFO("Looking Glass IDD Helper (" LG_VERSION_STR ")");

  char buffer[MAX_PATH];
  DWORD result = GetModuleFileNameA(NULL, buffer, MAX_PATH);
  if (result == 0)
  {
    DEBUG_ERROR("Failed to get the executable path");
    return EXIT_FAILURE;
  }  
  l_executable = buffer;

  int argc = 0;
  LPWSTR * wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::string> args;
  args.reserve(argc);
  for (int i = 0; i < argc; ++i)
  {
    size_t len = wcslen(wargv[i]);
    size_t bufSize = (len + 1) * 2;
    std::vector<char> buffer(bufSize);

    size_t converted = 0;
    errno_t err = wcstombs_s(&converted, buffer.data(), bufSize, wargv[i], bufSize - 1);
    if (err != 0)
    {
      DEBUG_ERROR("Conversion failed");
      return EXIT_FAILURE;
    }

    args.emplace_back(buffer.data());
  }
  LocalFree(wargv);

  if (argc == 1)
  {
    if (!HandleService())
      return EXIT_FAILURE;
    return EXIT_SUCCESS;
  }

  // child process
  if (argc != 2)
  {
    // the one and only value we should see is the exit event name
    DEBUG_ERROR("Invalid invocation");
    return EXIT_FAILURE;
  }

  l_exitEvent.Attach(OpenEvent(SYNCHRONIZE, FALSE, args[1].c_str()));
  if (!l_exitEvent.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to open the exit event: %s", args[1].c_str());
    return EXIT_FAILURE;
  }

  WNDCLASSEX wx = {};
  wx.cbSize        = sizeof(WNDCLASSEX);
  wx.lpfnWndProc   = DummyWndProc;
  wx.hInstance     = hInstance;
  wx.lpszClassName = "DUMMY_CLASS";
  wx.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
  wx.hIconSm       = LoadIcon(NULL, IDI_APPLICATION);
  wx.hCursor       = LoadCursor(NULL, IDC_ARROW);
  wx.hbrBackground = (HBRUSH)COLOR_APPWORKSPACE;
  ATOM aclass;
  if (!(aclass = RegisterClassEx(&wx)))
  {
    DEBUG_ERROR("Failed to register message window class");
    return EXIT_FAILURE;
  }

  HWND msgWnd = CreateWindowExA(0, MAKEINTATOM(aclass), NULL,
    0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);

  bool running = g_pipe.Init();
  while (running)
  {
    switch (MsgWaitForMultipleObjects(1, l_exitEvent.GetAddressOf(),
      FALSE, INFINITE, QS_ALLINPUT))
    {
      case WAIT_OBJECT_0:
        running = false;
        break;

      case WAIT_OBJECT_0 + 1:
      {
        MSG msg;
        if (GetMessage(&msg, NULL, 0, 0))
        {
          TranslateMessage(&msg);
          DispatchMessage(&msg);
        }
        break;
      }

      case WAIT_FAILED:
        DEBUG_ERROR_HR(GetLastError(), "MsgWaitForMultipleObjects Failed");
        running = false;
        break;
    }    
  }
  g_pipe.DeInit();

  DestroyWindow(msgWnd);
  return EXIT_SUCCESS;
}

bool HandleService()
{  
  SERVICE_TABLE_ENTRY DispatchTable[] =
  {
    { (char *)SVCNAME, SvcMain },
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

  UUID uuid;
  RPC_CSTR uuidStr;
  RPC_STATUS status = UuidCreate(&uuid);
  if (status != RPC_S_OK && status != RPC_S_UUID_LOCAL_ONLY && status != RPC_S_UUID_NO_ADDRESS)
  {
    DEBUG_ERROR("UuidCreate Failed: 0x%x", status);
    ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
    return;
  }

  if (UuidToString(&uuid, &uuidStr) == RPC_S_OK)
  {
    l_exitEventName = "Global\\";
    l_exitEventName += (const char *)uuidStr;
    RpcStringFree(&uuidStr);

    l_exitEvent.Attach(CreateEvent(NULL, FALSE, FALSE, l_exitEventName.c_str()));
    if (!l_exitEvent.IsValid())
    {
      DEBUG_ERROR_HR(GetLastError(), "CreateEvent Failed");
      ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
      return;
    }
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

  SetEvent(l_exitEvent.Get());
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

static bool EnablePriv(const char * name)
{
  return true;
}

static void DisablePriv(const char * name)
{

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
  si.lpDesktop   = (LPSTR)"WinSta0\\Default";

  char * cmdLine = NULL;
  char cmdBuf[128];
  if (l_exitEvent.IsValid())
  {
    snprintf(cmdBuf, sizeof(cmdBuf), "LGIddHelper.exe %s",
      l_exitEventName.c_str());
    cmdLine = cmdBuf;
  }

  if (!CreateProcessAsUserA(
    token.Get(),
    l_executable.c_str(),
    cmdLine,
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