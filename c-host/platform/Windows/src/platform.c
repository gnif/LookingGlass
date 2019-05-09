/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
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
#include "windows/platform.h"
#include "windows/mousehook.h"

#include <windows.h>
#include <setupapi.h>
#include <shellapi.h>

#include "interface/platform.h"
#include "common/debug.h"
#include "windows/debug.h"
#include "ivshmem/Public.h"

struct AppState
{
  int     argc;
  char ** argv;

  char         executable[MAX_PATH + 1];
  HANDLE       shmemHandle;
  bool         shmemOwned;
  IVSHMEM_MMAP shmemMap;
  HWND         messageWnd;
};

static struct AppState app =
{
  .shmemHandle = INVALID_HANDLE_VALUE,
  .shmemOwned  = false,
  .shmemMap    = {0}
};

struct osThreadHandle
{
  const char       * name;
  osThreadFunction   function;
  void             * opaque;
  HANDLE             handle;
  DWORD              threadID;

  int                resultCode;
};

LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  switch(msg)
  {
    case WM_CLOSE:
      mouseHook_remove();
      DestroyWindow(hwnd);
      break;

    case WM_DESTROY:
      PostQuitMessage(0);
      break;

    case WM_CALL_FUNCTION:
    {
      struct MSG_CALL_FUNCTION * cf = (struct MSG_CALL_FUNCTION *)lParam;
      return cf->fn(cf->wParam, cf->lParam);
    }

    default:
      return DefWindowProc(hwnd, msg, wParam, lParam);
  }
  return 0;
}

static int appThread(void * opaque)
{
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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
  int                              result = 0;
  HDEVINFO                         deviceInfoSet;
  PSP_DEVICE_INTERFACE_DETAIL_DATA infData = NULL;
  SP_DEVICE_INTERFACE_DATA         deviceInterfaceData;

  // convert the command line to the standard argc and argv
  LPWSTR * wargv = CommandLineToArgvW(GetCommandLineW(), &app.argc);
  app.argv = malloc(sizeof(char *) * app.argc);
  for(int i = 0; i < app.argc; ++i)
  {
    const size_t s = (wcslen(wargv[i])+1) * 2;
    size_t unused;
    app.argv[i] = malloc(s);
    wcstombs_s(&unused, app.argv[i], s, wargv[i], _TRUNCATE);
  }
  LocalFree(wargv);

  GetModuleFileName(NULL, app.executable, sizeof(app.executable));

  // redirect stderr to a file
  {
    char tempPath[MAX_PATH+1];
    GetTempPathA(sizeof(tempPath), tempPath);
    int len = snprintf(NULL, 0, "%slooking-glass-host.txt", tempPath);
    char * path = malloc(len + 1);
    sprintf(path, "%slooking-glass-host.txt", tempPath);
    freopen(path, "a", stderr);
    free(path);
  }

  // always flush stderr
  setbuf(stderr, NULL);

  deviceInfoSet = SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES | DIGCF_DEVICEINTERFACE);
  memset(&deviceInterfaceData, 0, sizeof(SP_DEVICE_INTERFACE_DATA));
  deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

  if (SetupDiEnumDeviceInterfaces(deviceInfoSet, NULL, &GUID_DEVINTERFACE_IVSHMEM, 0, &deviceInterfaceData) == FALSE)
  {
    DWORD error = GetLastError();
    if (error == ERROR_NO_MORE_ITEMS)
    {
      DEBUG_WINERROR("Unable to enumerate the device, is it attached?", error);
      result = -1;
      goto finish;
    }

    DEBUG_WINERROR("SetupDiEnumDeviceInterfaces failed", error);
    result  = -1;
    goto finish;
  }

  DWORD reqSize = 0;
  SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData, NULL, 0, &reqSize, NULL);
  if (!reqSize)
  {
    DEBUG_WINERROR("SetupDiGetDeviceInterfaceDetail", GetLastError());
    result = -1;
    goto finish;
  }

  infData         = (PSP_DEVICE_INTERFACE_DETAIL_DATA)calloc(reqSize, 1);
  infData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
  if (!SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData, infData, reqSize, NULL, NULL))
  {
    free(infData);
    DEBUG_WINERROR("SetupDiGetDeviceInterfaceDetail", GetLastError());
    result = -1;
    goto finish;
  }

  app.shmemHandle = CreateFile(infData->DevicePath, 0, 0, NULL, OPEN_EXISTING, 0, 0);
  if (app.shmemHandle == INVALID_HANDLE_VALUE)
  {
    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    free(infData);
    DEBUG_WINERROR("CreateFile returned INVALID_HANDLE_VALUE", GetLastError());
    result = -1;
    goto finish;
  }

  free(infData);
  SetupDiDestroyDeviceInfoList(deviceInfoSet);

  // setup a handler for ctrl+c
  SetConsoleCtrlHandler(CtrlHandler, TRUE);

  // create a message window so that our message pump works
  WNDCLASSEX wx    = {};
  wx.cbSize        = sizeof(WNDCLASSEX);
  wx.lpfnWndProc   = DummyWndProc;
  wx.hInstance     = hInstance;
  wx.lpszClassName = "DUMMY_CLASS";
  if (!RegisterClassEx(&wx))
  {
    DEBUG_ERROR("Failed to register message window class");
    result = -1;
    goto finish_shmem;
  }
  app.messageWnd = CreateWindowEx(0, "DUMMY_CLASS", "DUMMY_NAME", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);

  // create the application thread
  osThreadHandle * thread;
  if (!os_createThread("appThread", appThread, NULL, &thread))
  {
    DEBUG_ERROR("Failed to create the main application thread");
    result = -1;
    goto finish_shmem;
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
      result = -1;
      goto shutdown;
    }

    DEBUG_INFO("Platform shutdown");
    break;
  }

shutdown:
  app_quit();
  if (!os_joinThread(thread, &result))
  {
    DEBUG_ERROR("Failed to join the main application thread");
    result = -1;
  }
finish_shmem:
  os_shmemUnmap();
  CloseHandle(app.shmemHandle);
finish:

  for(int i = 0; i < app.argc; ++i)
    free(app.argv[i]);
  free(app.argv);

  return result;
}

const char * os_getExecutable()
{
  return app.executable;
}

unsigned int os_shmemSize()
{
  IVSHMEM_SIZE size;
  if (!DeviceIoControl(app.shmemHandle, IOCTL_IVSHMEM_REQUEST_SIZE, NULL, 0, &size, sizeof(IVSHMEM_SIZE), NULL, NULL))
  {
    DEBUG_WINERROR("DeviceIoControl Failed", GetLastError());
    return 0;
  }

  return (unsigned int)size;
}

bool os_shmemMmap(void **ptr)
{
  if (app.shmemOwned)
  {
    *ptr = app.shmemMap.ptr;
    return true;
  }

  memset(&app.shmemMap, 0, sizeof(IVSHMEM_MMAP));
  if (!DeviceIoControl(
    app.shmemHandle,
    IOCTL_IVSHMEM_REQUEST_MMAP,
    NULL, 0,
    &app.shmemMap, sizeof(IVSHMEM_MMAP),
    NULL, NULL))
  {
    DEBUG_WINERROR("DeviceIoControl Failed", GetLastError());
    return false;
  }

  *ptr = app.shmemMap.ptr;
  app.shmemOwned = true;
  return true;
}

void os_shmemUnmap()
{
  if (!app.shmemOwned)
    return;

  if (!DeviceIoControl(app.shmemHandle, IOCTL_IVSHMEM_RELEASE_MMAP, NULL, 0, NULL, 0, NULL, NULL))
    DEBUG_WINERROR("DeviceIoControl failed", GetLastError());
  else
    app.shmemOwned = false;
}

static DWORD WINAPI threadWrapper(LPVOID lpParameter)
{
  osThreadHandle * handle = (osThreadHandle *)lpParameter;
  handle->resultCode = handle->function(handle->opaque);
  return 0;
}

bool os_createThread(const char * name, osThreadFunction function, void * opaque, osThreadHandle ** handle)
{
  *handle             = (osThreadHandle *)malloc(sizeof(osThreadHandle));
  (*handle)->name     = name;
  (*handle)->function = function;
  (*handle)->opaque   = opaque;
  (*handle)->handle   = CreateThread(NULL, 0, threadWrapper, *handle, 0, &(*handle)->threadID);

  if (!(*handle)->handle)
  {
    free(*handle);
    *handle = NULL;
    DEBUG_WINERROR("CreateThread failed", GetLastError());
    return false;
  }

  return true;
}

bool os_joinThread(osThreadHandle * handle, int * resultCode)
{
  while(true)
  {
    switch(WaitForSingleObject(handle->handle, INFINITE))
    {
      case WAIT_OBJECT_0:
        if (resultCode)
          *resultCode = handle->resultCode;
        CloseHandle(handle->handle);
        free(handle);
        return true;

      case WAIT_ABANDONED:
      case WAIT_TIMEOUT:
        continue;

      case WAIT_FAILED:
        DEBUG_WINERROR("Wait for thread failed", GetLastError());
        CloseHandle(handle->handle);
        free(handle);
        return false;
    }

    break;
  }

  DEBUG_WINERROR("Unknown failure waiting for thread", GetLastError());
  return false;
}

osEventHandle * os_createEvent(bool autoReset)
{
  HANDLE event = CreateEvent(NULL, autoReset ? FALSE : TRUE, FALSE, NULL);
  if (!event)
  {
    DEBUG_WINERROR("Failed to create the event", GetLastError());
    return NULL;
  }

  return (osEventHandle*)event;
}

osEventHandle * os_wrapEvent(HANDLE event)
{
  return (osEventHandle*)event;
}

void os_freeEvent(osEventHandle * handle)
{
  CloseHandle((HANDLE)handle);
}

bool os_waitEvent(osEventHandle * handle, unsigned int timeout)
{
  const DWORD to = (timeout == TIMEOUT_INFINITE) ? INFINITE : (DWORD)timeout;
  while(true)
  {
    switch(WaitForSingleObject((HANDLE)handle, to))
    {
      case WAIT_OBJECT_0:
        return true;

      case WAIT_ABANDONED:
        continue;

      case WAIT_TIMEOUT:
        if (timeout == TIMEOUT_INFINITE)
          continue;

        return false;

      case WAIT_FAILED:
        DEBUG_WINERROR("Wait for event failed", GetLastError());
        return false;
    }

    DEBUG_ERROR("Unknown wait event return code");
    return false;
  }
}

bool os_waitEvents(osEventHandle * handles[], int count, bool waitAll, unsigned int timeout)
{
  const DWORD to = (timeout == TIMEOUT_INFINITE) ? INFINITE : (DWORD)timeout;
  while(true)
  {
    DWORD result = WaitForMultipleObjects(count, (HANDLE*)handles, waitAll, to);
    if (result >= WAIT_OBJECT_0 && result < count)
    {
      // null non signalled events from the handle list
      for(int i = 0; i < count; ++i)
        if (i != result && !os_waitEvent(handles[i], 0))
          handles[i] = NULL;
      return true;
    }

    if (result >= WAIT_ABANDONED_0 && result - WAIT_ABANDONED_0 < count)
      continue;

    switch(result)
    {
      case WAIT_TIMEOUT:
        if (timeout == TIMEOUT_INFINITE)
          continue;

        return false;

      case WAIT_FAILED:
        DEBUG_WINERROR("Wait for event failed", GetLastError());
        return false;
    }

    DEBUG_ERROR("Unknown wait event return code");
    return false;
  }
}

bool os_signalEvent(osEventHandle * handle)
{
  return SetEvent((HANDLE)handle);
}

bool os_resetEvent(osEventHandle * handle)
{
  return ResetEvent((HANDLE)handle);
}