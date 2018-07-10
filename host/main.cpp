/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
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

#include "common/debug.h"
#include "vendor/getopt/getopt.h"

#include "CrashHandler.h"
#include "TraceUtil.h"
#include "CaptureFactory.h"
#include "Service.h"

#include <io.h>
#include <fcntl.h> 
#include <iostream>

int parseArgs(struct StartupArgs & args);
int run(struct StartupArgs & args);

void doHelp();
void doLicense();

bool consoleActive = false;
void setupConsole();
bool avxSupport();

struct StartupArgs
{
  bool foreground;
  const char * captureDevice;
  CaptureOptions captureOptions;
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR szCmdParam, int iCmdShow)
{
  CrashHandler::Initialize();
  TraceUtil::Initialize();
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
  bool avx = avxSupport();
  if (!avx)
  {
	  setupConsole();
	  fprintf(stderr, "Warning: AVX appears to not be supported. Looking Glass may not run. Check CPU configuration.\n");
  }

  struct StartupArgs args;
  args.foreground = false;
  args.captureDevice = NULL;

  int ret = parseArgs(args);
  if (ret == 0)
  {
    ret = run(args);
    if (ret != 0)
    {
      if (!args.foreground)
      {
        setupConsole();
        fprintf(stderr, "An error occurred, re-run in forground mode (-f) for more information\n");
      }
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

int run(struct StartupArgs & args)
{
  if (args.foreground)
    setupConsole();

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

  Service *svc = svc->Get();
  if (!svc->Initialize(captureDevice))
    return -1;

  while (true)
  {
    if (!svc->Process())
      break;
  }

  svc->DeInitialize();
  return 0;
}

int parseArgs(struct StartupArgs & args)
{
  int c;
  while((c = getopt(__argc, __argv, "hc:o:fl")) != -1)
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
    "  -l  License information\n",
    app,
    app
  );
}

void doLicense()
{
  setupConsole();
  fprintf(stderr,
    "Looking Glass - KVM FrameRelay (KVMFR) Client\n"
    "Copyright(C) 2017 Geoffrey McRae <geoff@hostfission.com>\n"
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

bool avxSupport() 
{
	int cpuInfo[4];
	__cpuid(cpuInfo, 1);

	bool osUsesXSAVE_XRSTORE = cpuInfo[2] & (1 << 27) || false;
	bool cpuAVXSuport = cpuInfo[2] & (1 << 28) || false;

	if (osUsesXSAVE_XRSTORE && cpuAVXSuport)
	{
		unsigned long long xcrFeatureMask = _xgetbv(_XCR_XFEATURE_ENABLED_MASK);
		return (xcrFeatureMask & 0x6) == 0x6;
	}

	return false;
}