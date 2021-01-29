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

#include "platform.h"
#include "service.h"
#include "windows/mousehook.h"

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <fcntl.h>

#include "interface/platform.h"
#include "common/debug.h"
#include "common/windebug.h"
#include "common/option.h"
#include "common/locking.h"
#include "common/thread.h"

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
};

static struct AppState app = {0};
HWND MessageHWND;

// undocumented API to adjust the system timer resolution (yes, its a nasty hack)
typedef NTSTATUS (__stdcall *ZwSetTimerResolution_t)(ULONG RequestedResolution, BOOLEAN Set, PULONG ActualResolution);
static ZwSetTimerResolution_t ZwSetTimerResolution = NULL;

// linux mingw64 is missing this
#ifndef MSGFLT_RESET
  #define MSGFLT_RESET (0)
  #define MSGFLT_ALLOW (1)
  #define MSGFLT_DISALLOW (2)
#endif
typedef WINBOOL WINAPI (*PChangeWindowMessageFilterEx)(HWND hwnd, UINT message, DWORD action, void * pChangeFilterStruct);
PChangeWindowMessageFilterEx _ChangeWindowMessageFilterEx = NULL;

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

LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  switch(msg)
  {
    case WM_DESTROY:
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
          else
          {
            /* If LG is running as SYSTEM, ShellExecute would launch a process
             * as the SYSTEM user also, for security we will just show the file
             * location instead */
            //ShellExecute(NULL, NULL, logFile, NULL, NULL, SW_SHOWNORMAL);
            MessageBoxA(hwnd, logFile, "Log File Location", MB_OK | MB_ICONINFORMATION);
          }
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

  Shell_NotifyIcon(NIM_DELETE, &app.iconData);
  SendMessage(app.messageWnd, WM_DESTROY, 0, 0);
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
    {0}
  };

  option_register(options);

  // setup a handler for ctrl+c
  SetConsoleCtrlHandler(CtrlHandler, TRUE);

  // enable high DPI awareness
  // this is required for DXGI 1.5 support to function and also capturing desktops with high DPI
  DECLARE_HANDLE(DPI_AWARENESS_CONTEXT);
  #define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2  ((DPI_AWARENESS_CONTEXT)-4)
  typedef BOOL (*User32_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT value);

  HMODULE user32 = GetModuleHandle("user32.dll");
  User32_SetProcessDpiAwarenessContext fn;
  fn = (User32_SetProcessDpiAwarenessContext)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
  if (fn)
    fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

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
  _ChangeWindowMessageFilterEx = (PChangeWindowMessageFilterEx)GetProcAddress(user32, "ChangeWindowMessageFilterEx");
  if (_ChangeWindowMessageFilterEx)
    _ChangeWindowMessageFilterEx(app.messageWnd, app.trayRestartMsg, MSGFLT_ALLOW, NULL);

  // set the global
  MessageHWND = app.messageWnd;

  app.trayMenu = CreatePopupMenu();
  AppendMenu(app.trayMenu, MF_STRING   , ID_MENU_SHOW_LOG, "Log File Location");
  AppendMenu(app.trayMenu, MF_SEPARATOR, 0               , NULL               );
  AppendMenu(app.trayMenu, MF_STRING   , ID_MENU_EXIT    , "Exit"             );

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

bool app_init(void)
{
  const char * logFile   = option_get_string("os", "logFile"  );

  // redirect stderr to a file
  if (logFile && strcmp(logFile, "stderr") != 0)
    freopen(logFile, "a", stderr);

  // always flush stderr
  setbuf(stderr, NULL);

  // Increase the timer resolution
  ZwSetTimerResolution = (ZwSetTimerResolution_t)GetProcAddress(GetModuleHandle("ntdll.dll"), "ZwSetTimerResolution");
  if (ZwSetTimerResolution)
  {
    ULONG actualResolution;
    ZwSetTimerResolution(1, true, &actualResolution);
    DEBUG_INFO("System timer resolution: %.2f ns", (float)actualResolution / 100.0f);
  }

  // get the performance frequency for spinlocks
  QueryPerformanceFrequency(&app.perfFreq);

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
