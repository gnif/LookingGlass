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

#pragma once

#define W32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>

#include "IVSHMEM.h"
#include "ICapture.h"
#include "common/debug.h"

#define MAX_FRAMES 2

enum ProcessStatus
{
  PROCESS_STATUS_OK,
  PROCESS_STATUS_RETRY,
  PROCESS_STATUS_ERROR
};

class Service
{
public:
  static Service & Instance()
  {
    static Service service;
    return service;
  }

  static void InstallHook()
  {
    if (Instance().m_mouseHook)
      RemoveHook();
    Instance().m_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, _LowLevelMouseProc, NULL, 0);
  }

  static void RemoveHook()
  {
    if (Instance().m_mouseHook)
    {
      UnhookWindowsHookEx(Instance().m_mouseHook);
      Instance().m_mouseHook = NULL;
    }
  }

  static void SetDevice(PCI_DEVICE dev)
  {
    s_dev = dev;
  }

  bool Initialize(ICapture * captureDevice);
  void DeInitialize();
  ProcessStatus Process();

private:
  bool InitPointers();

  int m_tryTarget;
  int m_lastTryCount;

  Service();
  ~Service();

  HHOOK m_mouseHook;
  LRESULT LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
  static LRESULT WINAPI _LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) { return Service::Instance().LowLevelMouseProc(nCode, wParam, lParam); }

  bool ReInit(volatile char * flags);

  bool       m_initialized;
  bool       m_running;
  DWORD      m_consoleSessionID;
  uint8_t  * m_memory;
  IVSHMEM  * m_ivshmem;
  HANDLE     m_timer;
  ICapture * m_capture;

  KVMFRHeader * m_shmHeader;

  bool          m_haveFrame;
  uint8_t     * m_frame[MAX_FRAMES];
  size_t        m_frameSize;
  uint64_t      m_dataOffset[MAX_FRAMES];
  int           m_frameIndex;
  static PCI_DEVICE  s_dev;

  static DWORD WINAPI _CursorThread(LPVOID lpParameter) { return Service::Instance().CursorThread(); }
  DWORD CursorThread();

  HANDLE           m_cursorThread;
  HANDLE           m_cursorEvent;
  CRITICAL_SECTION m_cursorCS;
  size_t           m_cursorDataSize;
  uint8_t        * m_cursorData;
  uint64_t         m_cursorOffset;
};