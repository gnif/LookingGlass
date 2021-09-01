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

#include "platform.h"
#include "service.h"
#include "windows/mousehook.h"

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <fcntl.h>
#include <powrprof.h>
#include <ntstatus.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <winternl.h>

#include "interface/platform.h"
#include "common/debug.h"
#include "common/windebug.h"
#include "common/option.h"
#include "common/locking.h"
#include "common/thread.h"
#include "common/time.h"

#define ID_MENU_SHOW_LOG 3000
#define ID_MENU_EXIT     3001
#define LOG_NAME         "looking-glass-host.txt"

struct AppState
{
  LARGE_INTEGER perfFreq;
  HINSTANCE hInst;

  int     argc;
  char ** argv;

  char           executable[MAX_PATH + 1];
  char           systemLogDir[MAX_PATH];
  HWND           messageWnd;
  NOTIFYICONDATA iconData;
  UINT           trayRestartMsg;
  HMENU          trayMenu;
  HANDLE         exitWait;
};

static struct AppState app = {0};
HWND MessageHWND;

CreateProcessAsUserA_t f_CreateProcessAsUserA = NULL;

bool windowsSetupAPI(void)
{
  /* first look in kernel32.dll */
  HMODULE mod;

  mod = GetModuleHandleA("kernel32.dll");
  if (mod)
  {
    f_CreateProcessAsUserA = (CreateProcessAsUserA_t)
      GetProcAddress(mod, "CreateProcessAsUserA");
    if (f_CreateProcessAsUserA)
      return true;
  }

  mod = GetModuleHandleA("advapi32.dll");
  if (mod)
  {
    f_CreateProcessAsUserA = (CreateProcessAsUserA_t)
      GetProcAddress(mod, "CreateProcessAsUserA");
    if (f_CreateProcessAsUserA)
      return true;
  }

  return false;
}

static void RegisterTrayIcon(void)
{
  // register our TrayIcon
  if (!app.iconData.cbSize)
  {
    app.iconData.cbSize           = sizeof(NOTIFYICONDATA);
    app.iconData.hWnd             = app.messageWnd;
    app.iconData.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    app.iconData.uCallbackMessage = WM_TRAYICON;
    strncpy(app.iconData.szTip, "Looking Glass (host)", sizeof(app.iconData.szTip));
    app.iconData.hIcon            = LoadIcon(app.hInst, IDI_APPLICATION);
  }
  Shell_NotifyIcon(NIM_ADD, &app.iconData);
}

// This function executes notepad as the logged in user, and therefore is secure to use.
static bool OpenLogFile(const char * logFile)
{
  bool result = false;

  DWORD console = WTSGetActiveConsoleSessionId();
  if (console == 0xFFFFFFFF)
  {
    DEBUG_WINERROR("Failed to get active console session ID", GetLastError());
    return false;
  }

  WTS_CONNECTSTATE_CLASS * state;
  DWORD size;
  if (!WTSQuerySessionInformationA(WTS_CURRENT_SERVER_HANDLE, console, WTSConnectState,
      (LPSTR *) &state, &size))
  {
    DEBUG_WINERROR("Failed to get session information", GetLastError());
    return false;
  }

  if (*state != WTSActive)
  {
    DEBUG_WINERROR("Will not open log file because user is not logged in", GetLastError());
    WTSFreeMemory(state);
    return false;
  }
  WTSFreeMemory(state);

  char system32[MAX_PATH];
  if (!GetSystemDirectoryA(system32, MAX_PATH))
  {
    DEBUG_WINERROR("Failed to get system directory", GetLastError());
    return false;
  }

  if (!f_CreateProcessAsUserA && !windowsSetupAPI())
  {
    DEBUG_WINERROR("Failed to get CreateProcessAsUserA", GetLastError());
    return false;
  }

  HANDLE hToken;
  if (!WTSQueryUserToken(console, &hToken))
  {
    DEBUG_WINERROR("Failed to get active console session user token", GetLastError());
    return false;
  }

  LPVOID env;
  if (!CreateEnvironmentBlock(&env, hToken, FALSE))
  {
    DEBUG_WINERROR("Failed to create environment", GetLastError());
    goto fail_token;
  }

  char notepad[MAX_PATH];
  PathCombineA(notepad, system32, "notepad.exe");

  char cmdline[MAX_PATH + 10];
  snprintf(cmdline, sizeof(cmdline), "notepad \"%s\"", logFile);

  STARTUPINFO si = { .cb = sizeof(STARTUPINFO) };
  PROCESS_INFORMATION pi = {0};
  if (!f_CreateProcessAsUserA(
      hToken,
      notepad,
      cmdline,
      NULL,
      NULL,
      FALSE,
      CREATE_UNICODE_ENVIRONMENT,
      env,
      os_getDataPath(),
      &si,
      &pi
    ))
  {
    DEBUG_WINERROR("Failed to open log file", GetLastError());
    goto fail_env;
  }

  result = true;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

fail_env:
  DestroyEnvironmentBlock(env);
fail_token:
  CloseHandle(hToken);
  return result;
}

LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  switch(msg)
  {
    case WM_DESTROY:
      Shell_NotifyIcon(NIM_DELETE, &app.iconData);
      MessageHWND = NULL;
      PostQuitMessage(0);
      break;

    case WM_CALL_FUNCTION:
    {
      struct MSG_CALL_FUNCTION * cf = (struct MSG_CALL_FUNCTION *)lParam;
      return cf->fn(cf->wParam, cf->lParam);
    }

    case WM_TRAYICON:
    {
      if (lParam == WM_RBUTTONDOWN)
      {
        POINT curPoint;
        GetCursorPos(&curPoint);
        SetForegroundWindow(hwnd);
        UINT clicked = TrackPopupMenu(
          app.trayMenu,
          TPM_RETURNCMD | TPM_NONOTIFY,
          curPoint.x,
          curPoint.y,
          0,
          hwnd,
          NULL
        );

             if (clicked == ID_MENU_EXIT    ) app_quit();
        else if (clicked == ID_MENU_SHOW_LOG)
        {
          const char * logFile = option_get_string("os", "logFile");
          if (strcmp(logFile, "stderr") == 0)
            DEBUG_INFO("Ignoring request to open the logFile, logging to stderr");
          else if (!OpenLogFile(logFile))
            MessageBoxA(hwnd, logFile, "Log File Location", MB_OK | MB_ICONINFORMATION);
        }
      }
      break;
    }

    default:
      if (msg == app.trayRestartMsg)
        RegisterTrayIcon();
      break;
  }

  return DefWindowProc(hwnd, msg, wParam, lParam);
}

static int appThread(void * opaque)
{
  RegisterTrayIcon();
  int result = app_main(app.argc, app.argv);
  SendMessage(app.messageWnd, WM_CLOSE, 0, 0);
  return result;
}

LRESULT sendAppMessage(UINT Msg, WPARAM wParam, LPARAM lParam)
{
  return SendMessage(app.messageWnd, Msg, wParam, lParam);
}

static BOOL WINAPI CtrlHandler(DWORD dwCtrlType)
{
  if (dwCtrlType == CTRL_C_EVENT)
  {
    SendMessage(app.messageWnd, WM_CLOSE, 0, 0);
    return TRUE;
  }

  return FALSE;
}

const char *getSystemLogDirectory(void)
{
  return app.systemLogDir;
}

static void populateSystemLogDirectory()
{
  char programData[MAX_PATH];
  if (GetEnvironmentVariableA("ProgramData", programData, sizeof(programData)) &&
      PathIsDirectoryA(programData))
  {
    if (!PathCombineA(app.systemLogDir, programData, "Looking Glass (host)"))
      goto fail;

    if (!PathIsDirectoryA(app.systemLogDir) && !CreateDirectoryA(app.systemLogDir, NULL))
      goto fail;

    return;
  }
fail:
  strcpy(app.systemLogDir, "");
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
  // initialize for DEBUG_* macros
  debug_init();

  // convert the command line to the standard argc and argv
  LPWSTR * wargv = CommandLineToArgvW(GetCommandLineW(), &app.argc);
  app.argv = malloc(sizeof(char *) * app.argc);
  for(int i = 0; i < app.argc; ++i)
  {
    const size_t s = (wcslen(wargv[i])+1) * 2;
    app.argv[i] = malloc(s);
    wcstombs(app.argv[i], wargv[i], s);
  }
  LocalFree(wargv);

  GetModuleFileName(NULL, app.executable, sizeof(app.executable));
  populateSystemLogDirectory();

  if (HandleService(app.argc, app.argv))
    return LG_HOST_EXIT_FAILED;

  /* this is a bit of a hack but without this --help will produce no output in a windows command prompt */
  if (!IsDebuggerPresent() && AttachConsole(ATTACH_PARENT_PROCESS))
  {
    HANDLE std_err = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE std_out = GetStdHandle(STD_OUTPUT_HANDLE);
    int std_err_fd = _open_osfhandle((intptr_t)std_err, _O_TEXT);
    int std_out_fd = _open_osfhandle((intptr_t)std_out, _O_TEXT);

    if (std_err_fd > 0)
      *stderr = *_fdopen(std_err_fd, "w");

    if  (std_out_fd > 0)
      *stdout = *_fdopen(std_out_fd, "w");
  }

  int result = 0;
  app.hInst = hInstance;

  char logFilePath[MAX_PATH];
  if (!PathCombineA(logFilePath, app.systemLogDir, LOG_NAME))
    strcpy(logFilePath, LOG_NAME);

  struct Option options[] =
  {
    {
      .module         = "os",
      .name           = "logFile",
      .description    = "The log file to write to",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = logFilePath
    },
    {
      .module         = "os",
      .name           = "exitEvent",
      .description    = "Exit when the specified event is signaled",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = ""
    },
    {0}
  };

  option_register(options);

  // setup a handler for ctrl+c
  SetConsoleCtrlHandler(CtrlHandler, TRUE);

  // create a message window so that our message pump works
  WNDCLASSEX wx    = {};
  wx.cbSize        = sizeof(WNDCLASSEX);
  wx.lpfnWndProc   = DummyWndProc;
  wx.hInstance     = hInstance;
  wx.lpszClassName = "DUMMY_CLASS";
  wx.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
  wx.hIconSm       = LoadIcon(NULL, IDI_APPLICATION);
  wx.hCursor       = LoadCursor(NULL, IDC_ARROW);
  wx.hbrBackground = (HBRUSH)COLOR_APPWORKSPACE;
  ATOM class;
  if (!(class = RegisterClassEx(&wx)))
  {
    DEBUG_ERROR("Failed to register message window class");
    result = LG_HOST_EXIT_FAILED;
    goto finish;
  }

  app.trayRestartMsg = RegisterWindowMessage("TaskbarCreated");

  app.messageWnd = CreateWindowEx(0, MAKEINTATOM(class), NULL, 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);

  // this is needed so that unprivileged processes can send us this message
  ChangeWindowMessageFilterEx(app.messageWnd, app.trayRestartMsg, MSGFLT_ALLOW, NULL);

  // set the global
  MessageHWND = app.messageWnd;

  app.trayMenu = CreatePopupMenu();
  AppendMenu(app.trayMenu, MF_STRING   , ID_MENU_SHOW_LOG, "Open Log File");
  AppendMenu(app.trayMenu, MF_SEPARATOR, 0               , NULL           );
  AppendMenu(app.trayMenu, MF_STRING   , ID_MENU_EXIT    , "Exit"         );

  // create the application thread
  LGThread * thread;
  if (!lgCreateThread("appThread", appThread, NULL, &thread))
  {
    DEBUG_ERROR("Failed to create the main application thread");
    result = LG_HOST_EXIT_FAILED;
    goto finish;
  }

  while(true)
  {
    MSG  msg;
    BOOL bRet = GetMessage(&msg, NULL, 0, 0);
    if (bRet > 0)
    {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
      continue;
    }
    else if (bRet < 0)
    {
      DEBUG_ERROR("Unknown error from GetMessage");
      result = LG_HOST_EXIT_FAILED;
      goto shutdown;
    }

    break;
  }

shutdown:
  DestroyMenu(app.trayMenu);
  app_shutdown();
  UnregisterWait(app.exitWait);

  if (!lgJoinThread(thread, &result))
  {
    DEBUG_ERROR("Failed to join the main application thread");
    result = LG_HOST_EXIT_FAILED;
  }

finish:

  for(int i = 0; i < app.argc; ++i)
    free(app.argv[i]);
  free(app.argv);

  return result;
}

typedef enum _D3DKMT_SCHEDULINGPRIORITYCLASS
{
  D3DKMT_SCHEDULINGPRIORITYCLASS_IDLE         = 0,
  D3DKMT_SCHEDULINGPRIORITYCLASS_BELOW_NORMAL = 1,
  D3DKMT_SCHEDULINGPRIORITYCLASS_NORMAL       = 2,
  D3DKMT_SCHEDULINGPRIORITYCLASS_ABOVE_NORMAL = 3,
  D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH         = 4,
  D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME     = 5,
}
D3DKMT_SCHEDULINGPRIORITYCLASS;

NTSTATUS APIENTRY D3DKMTSetProcessSchedulingPriorityClass(
  _In_ HANDLE, _In_ D3DKMT_SCHEDULINGPRIORITYCLASS
);

void boostPriority(void)
{
  if (FAILED(D3DKMTSetProcessSchedulingPriorityClass(GetCurrentProcess(),
        D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME)))
  {
    DEBUG_WARN("Failed to set realtime GPU priority.");
    DEBUG_INFO("This is not a failure, please do not report this as an issue.");
    DEBUG_INFO("To fix this, install and run the Looking Glass host as a service.");
    DEBUG_INFO("looking-glass-host.exe InstallService");
  }
}

void CALLBACK exitEventCallback(PVOID opaque, BOOLEAN timedOut)
{
  DEBUG_INFO("Received exit event");
  SendMessage(app.messageWnd, WM_CLOSE, 0, 0);
}

bool app_init(void)
{
  const char * logFile = option_get_string("os", "logFile");

  // redirect stderr to a file
  if (logFile && strcmp(logFile, "stderr") != 0)
    freopen(logFile, "a", stderr);

  // always flush stderr
  setbuf(stderr, NULL);

  windowsSetTimerResolution();

  // get the performance frequency for spinlocks
  QueryPerformanceFrequency(&app.perfFreq);

  // try to boost the scheduler priority
  boostPriority();

  // open exit signaling event
  HANDLE exitEvent = NULL;
  const char * exitEventName = option_get_string("os", "exitEvent");
  if (exitEventName && exitEventName[0])
  {
    exitEvent = OpenEvent(SYNCHRONIZE, FALSE, exitEventName);
    if (!exitEvent)
    {
      DEBUG_WINERROR("Failed to open exitEvent", GetLastError());
      DEBUG_INFO("Exit event name: %s", exitEventName);
    }
  }

  if (exitEvent &&
      !RegisterWaitForSingleObject(&app.exitWait, exitEvent, exitEventCallback, NULL,
        INFINITE, WT_EXECUTEONLYONCE))
    DEBUG_WINERROR("Failed to register wait for exit event", GetLastError());

  return true;
}

const char * os_getExecutable(void)
{
  return app.executable;
}

const char * os_getDataPath(void)
{
  static char path[MAX_PATH] = { 0 };
  if (!path[0])
  {
    if (!GetModuleFileName(NULL, path, MAX_PATH))
      return NULL;

    char *p = strrchr(path, '\\');
    if (!p)
      return NULL;

    ++p;
    *p = '\0';
  }
  return path;
}

HWND os_getMessageWnd(void)
{
  return app.messageWnd;
}

bool os_blockScreensaver()
{
  static bool      lastResult = false;
  static ULONGLONG lastCheck  = 0;

  ULONGLONG now = GetTickCount64();
  if (now - lastCheck >= 1000)
  {
    ULONG executionState;
    NTSTATUS status = CallNtPowerInformation(SystemExecutionState, NULL, 0,
      &executionState, sizeof executionState);

    if (status == STATUS_SUCCESS)
      lastResult = executionState & ES_DISPLAY_REQUIRED;
    else
      DEBUG_WINERROR("Failed to call CallNtPowerInformation(SystemExecutionState)",
        RtlNtStatusToDosError(status));
    lastCheck = now;
  }
  return lastResult;
}

void os_showMessage(const char * caption, const char * msg)
{
  MessageBoxA(NULL, msg, caption, MB_OK | MB_ICONINFORMATION);
}

bool os_hasSetCursorPos(void)
{
  return true;
}

void os_setCursorPos(int x, int y)
{
  SetCursorPos(x, y);
}
