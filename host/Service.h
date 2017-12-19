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

#pragma once

#define W32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>

#include "IVSHMEM.h"
#include "ICapture.h"

class Service
{
public:
  static Service * Get()
  {
    if (!m_instance)
      m_instance = new Service();
    return m_instance;
  }

  bool Initialize(ICapture * captureDevice);
  void DeInitialize();
  bool Process();

private:
  bool InitPointers();

  static Service * m_instance;

  Service();
  ~Service();

  bool       m_initialized;
  DWORD      m_consoleSessionID;
  uint8_t  * m_memory;
  IVSHMEM  * m_ivshmem;
  HANDLE     m_timer;
  ICapture * m_capture;

  KVMFRDetail   m_detail;
  KVMFRHeader * m_shmHeader;
  uint8_t     * m_frame[2];
  size_t        m_frameSize;
  uint64_t      m_dataOffset[2];
  int           m_frameIndex;

  size_t        m_cursorDataSize;
  uint8_t     * m_cursorData;
  uint64_t      m_cursorOffset;
};