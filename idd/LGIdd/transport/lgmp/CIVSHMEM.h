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

#pragma once

#include <Windows.h>
#include <SetupAPI.h>
#include <vector>

#include "common/KVMFRRecovery.h"

class CIVSHMEM
{
private:
  struct IVSHMEMData
  {
    SP_DEVINFO_DATA devInfoData;
    DWORD64         busAddr;
  };

  std::vector<struct IVSHMEMData> m_devices;
  HANDLE m_handle = INVALID_HANDLE_VALUE;
  size_t m_size   = 0;
  void * m_mem    = nullptr;

public:
  CIVSHMEM();
  ~CIVSHMEM();

  bool Init();
  bool Open();
  void Close();

  size_t GetFullSize    () const { return m_size; }
  size_t GetUsableSize  () const
  {
    if (m_size < KVMFR_R_REGION_SIZE)
      return 0;

    return m_size - KVMFR_R_REGION_SIZE;
  }

  void * GetMem         () const { return m_mem; }
  void * GetRecoveryMem () const
  {
    if (!m_mem || m_size < KVMFR_R_REGION_SIZE)
      return nullptr;

    return static_cast<uint8_t *>(m_mem) + GetUsableSize();
  }
};
