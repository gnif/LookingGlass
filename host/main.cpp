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

#include <windows.h>
#include <shlwapi.h>
#include <avrt.h>

#include "common/debug.h"
#include "getopt/getopt.h"

#include "CrashHandler.h"
#include "TraceUtil.h"
#include "CaptureFactory.h"
#include "Service.h"

#include <io.h>
#include <fcntl.h> 
#include <iostream>

int parseArgs(struct StartupArgs & args);
static DWORD WINAPI CaptureThread(LPVOID lpParameter);
int run();

void doHelp();
void doLicense();

bool running = true;
bool consoleActive = false;
void setupConsole();

extern "C" NTSYSAPI NTSTATUS NTAPI NtSetTimerResolution(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);

struct StartupArgs
{
  bool foreground;
  const char * captureDevice;
  CaptureOptions captureOptions;
};
struct StartupArgs args;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR szCmdParam, int iCmdShow)
{
  CrashHandler::Initialize();
  TraceUtil::Initialize();
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  args.foreground = false;
  args.captureDevice = NULL;
  int ret = parseArgs(args);
  if (ret != 0)
    fprintf(stderr, "Failed to parse command line arguments\n");
  else
  {
    if (args.foreground)
      setupConsole();

    Service::InstallHook();
    HANDLE captureThread = CreateThread(NULL, 0, CaptureThread, NULL, 0, NULL);
    while (running)
    {
      MSG msg;
      BOOL bRet = GetMessage(&msg, NULL, 0, 0);
      if (bRet == -1 || bRet == 0)
      {
        ret = msg.wParam;
        break;
      }
      DispatchMessage(&msg);
    }
    Service::RemoveHook();
    running = false;
    ret = WaitForSingleObject(captureThread, INFINITE);
    CloseHandle(captureThread);
  }

  if (ret != 0)
  {
    if (!args.foreground)
    {
      setupConsole();
      fprintf(stderr, "An error occurred, re-run in forground mode (-f) for more information\n");
    }
  }

  if (consoleActive)
  {
    fprintf(stderr, "\nPress enter to terminate...");
    fflush(stderr);
    getc(stdin);
  }

  return ret;
}

static DWORD WINAPI CaptureThread(LPVOID lpParameter)
{
  int ret = 0;
  while (running)
  {
    ret = run();
    if (ret != 0)
      break;
  }
  running = false;
  return ret;
}

int run()
{
  /* increase the system timer resolution */
  ULONG currentRes;
  NtSetTimerResolution(0, TRUE, &currentRes);

  /* boost our thread priority class as high as possible */
  SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);

  /* use MMCSS to boost our priority for capture */
  DWORD taskIndex = 0;
  HANDLE task = AvSetMmThreadCharacteristics(L"Capture", &taskIndex);
  if (!task || (AvSetMmThreadPriority(task, AVRT_PRIORITY_CRITICAL) == FALSE))
    DEBUG_WARN("Failed to boosted priority using MMCSS");

  ICapture * captureDevice;
  if (args.captureDevice == NULL)
    captureDevice = CaptureFactory::DetectDevice(&args.captureOptions);
  else
  {
    captureDevice = CaptureFactory::GetDevice(args.captureDevice, &args.captureOptions);
    if (!captureDevice)
    {
      setupConsole();
      fprintf(stderr, "Failed to configure requested capture device\n");
      return -1;
    }
  }

  if (!captureDevice)
  {
    setupConsole(); 
    fprintf(stderr, "Unable to configure a capture device\n");
    return -1;
  }

  Service &svc = Service::Instance();
  if (!svc.Initialize(captureDevice))
    return -1;

  int retry = 0;
  bool running = true;
  while (running)
  {
    switch (svc.Process())
    {
      case PROCESS_STATUS_OK:
        retry = 0;
        break;

      case PROCESS_STATUS_RETRY:
        if (retry++ == 3)
        {
          fprintf(stderr, "Too many consecutive retries, aborting");
          running = false;
        }
        break;

      case PROCESS_STATUS_ERROR:
        fprintf(stderr, "Capture process returned error");
        running = false;
    }
  }

  svc.DeInitialize();

  if (task)
    AvRevertMmThreadCharacteristics(task);

  return 0;
}

int parseArgs(struct StartupArgs & args)
{
  int c;
  while((c = getopt(__argc, __argv, "hc:o:fld:")) != -1)
  {
    switch (c)
    {
    case '?':
    case 'h':
      doHelp();
      return -1;

    case 'c':
    {
      const CaptureFactory::DeviceList deviceList = CaptureFactory::GetDevices();

      bool found = false;
      if (strcmp(optarg, "?") != 0)
      {
        for (CaptureFactory::DeviceList::const_iterator it = deviceList.begin(); it != deviceList.end(); ++it)
        {
          if (_strcmpi(optarg, (*it)->GetName()) == 0)
          {
            args.captureDevice = (*it)->GetName();
            found = true;
            break;
          }
        }

        if (!found)
        {
          setupConsole();
          fprintf(stderr, "Invalid capture device: %s\n\n", optarg);          
        }
      }

      if (!found)
      {
        setupConsole();
        fprintf(stderr, "Available Capture Devices:\n\n");
        for (CaptureFactory::DeviceList::const_iterator it = deviceList.begin(); it != deviceList.end(); ++it)
          fprintf(stderr, "  %s\n", (*it)->GetName());
        return -1;
      }
      break;
    }

    case 'o':
    {
      args.captureOptions.push_back(optarg);
      break;
    }

    case 'f':
      args.foreground = true;
      break;

    case 'l':
      doLicense();
      return -1;

    case 'd':
      if (optarg == NULL || strlen(optarg) == 0)
      {
        setupConsole();
        fprintf(stderr, "Device ID missing\n");
        return -1;
      }
      else if (*optarg == '?')
      {
        setupConsole();
        IVSHMEM::listDevices();
        return -1;
      }
      else
      {
        PCI_DEVICE dev;
        int cnt = sscanf_s(optarg, "%hhu,%hhu,%hhu", &dev.bus, &dev.addr, &dev.func);
        if (cnt == 3)
        {
          Service::SetDevice(dev);
        }
        else
        {
          setupConsole();
          fprintf(stderr, "Invalid Parameter\n");
          return -1;
        }
      }
      break;
    }
  }

  return 0;
}

void doHelp()
{
  setupConsole();
  const char *app = PathFindFileNameA(__argv[0]);
  fprintf(stderr,
    "Usage: %s [OPTION]...\n"
    "Example: %s -c ?\n"
    "\n"
    "  -h  Print out this help\n"
    "  -c  Specify the capture device to use or ? to list availble (device is probed if not specified)\n"
    "  -o  Option to pass to the capture device, may be specified multiple times for extra options\n"
    "  -f  Foreground mode\n"
    "  -l  License information\n"
    "  -d  Specify the IVSHMEM device with \"<bus>,<slot>,<function>\" or ? to list available\n",
    app,
    app
  );
}

void doLicense()
{
  setupConsole();
  fprintf(stderr,
    "Looking Glass - KVM FrameRelay (KVMFR) Client\n"
    "Copyright(C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>\n"
    "\n"
    "This program is free software; you can redistribute it and / or modify it under\n"
    "the terms of the GNU General Public License as published by the Free Software\n"
    "Foundation; either version 2 of the License, or (at your option) any later\n"
    "version.\n"
    "\n"
    "This program is distributed in the hope that it will be useful, but WITHOUT ANY\n"
    "WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A\n"
    "PARTICULAR PURPOSE.See the GNU General Public License for more details.\n"
    "\n"
    "You should have received a copy of the GNU General Public License along with\n"
    "this program; if not, write to the Free Software Foundation, Inc., 59 Temple\n"
    "Place, Suite 330, Boston, MA 02111 - 1307 USA\n"
  );
}

void setupConsole()
{
  if (consoleActive)
    return;

  HANDLE _handle;
  int    _conout;
  FILE * fp;

  AllocConsole();

  CONSOLE_SCREEN_BUFFER_INFO conInfo;
  GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &conInfo);
  conInfo.dwSize.Y = 500;
  SetConsoleScreenBufferSize(GetStdHandle(STD_OUTPUT_HANDLE), conInfo.dwSize);

  _handle = GetStdHandle(STD_INPUT_HANDLE);
  _conout = _open_osfhandle((intptr_t)_handle, _O_TEXT);
  fp = _fdopen(_conout, "r");
  freopen_s(&fp, "CONIN$", "r", stdin);

  _handle = GetStdHandle(STD_OUTPUT_HANDLE);
  _conout = _open_osfhandle((intptr_t)_handle, _O_TEXT);
  fp = _fdopen(_conout, "w");
  freopen_s(&fp, "CONOUT$", "w", stdout);

  _handle = GetStdHandle(STD_ERROR_HANDLE);
  _conout = _open_osfhandle((intptr_t)_handle, _O_TEXT);
  fp = _fdopen(_conout, "w");
  freopen_s(&fp, "CONOUT$", "w", stderr);

  std::ios::sync_with_stdio();
  std::wcout.clear();
  std::cout.clear();
  std::wcerr.clear();
  std::cerr.clear();
  std::wcin.clear();
  std::cin.clear();

  consoleActive = true;
}