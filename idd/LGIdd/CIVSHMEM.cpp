/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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

#include "CIVSHMEM.h"

#include <Windows.h>
#include <SetupAPI.h>
#include <algorithm>
#include <winioctl.h>

#include "CDebug.h"
#include "ivshmem/ivshmem.h"

CIVSHMEM::CIVSHMEM()
{
}

CIVSHMEM::~CIVSHMEM()
{
  if (m_handle == INVALID_HANDLE_VALUE)
    return;

  Close();
  CloseHandle(m_handle);
}

bool CIVSHMEM::Init()
{
  HDEVINFO                         devInfoSet;
  SP_DEVINFO_DATA                  devInfoData;
  SP_DEVICE_INTERFACE_DATA         devInterfaceData;
  PSP_DEVICE_INTERFACE_DETAIL_DATA infData = nullptr;

  devInfoSet = SetupDiGetClassDevs(&GUID_DEVINTERFACE_IVSHMEM, nullptr, nullptr,
    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

  devInfoData.cbSize      = sizeof(devInfoData);
  devInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

  m_devices.clear();
  for (int i = 0; SetupDiEnumDeviceInfo(devInfoSet, i, &devInfoData); ++i)
  {
    DWORD bus, addr;
    if (!SetupDiGetDeviceRegistryProperty(devInfoSet, &devInfoData, SPDRP_BUSNUMBER,
        nullptr, (BYTE*)&bus, sizeof(bus), nullptr))
      bus = 0xffff;

    if (!SetupDiGetDeviceRegistryProperty(devInfoSet, &devInfoData, SPDRP_ADDRESS,
        nullptr, (BYTE*)&addr, sizeof(addr), nullptr))
      addr = 0xffff;

    IVSHMEMData data;
    data.busAddr = ((DWORD64)bus) << 32 | addr;
    memcpy(&data.devInfoData, &devInfoData, sizeof(devInfoData));
    m_devices.push_back(data);
  }

  HRESULT hr = GetLastError();
  if (hr != ERROR_NO_MORE_ITEMS)
  {
    m_devices.clear();
    SetupDiDestroyDeviceInfoList(devInfoSet);
    DEBUG_ERROR_HR(hr, "Enumeration Failed");
    return false;
  }

  std::sort(m_devices.begin(), m_devices.end(),
      [](const IVSHMEMData & a, const IVSHMEMData & b) -> bool
    { return a.busAddr < b.busAddr; });


  HKEY hkeyLG;
  IVSHMEMData * device = nullptr;
  DWORD shmDevice = 0;

  if (RegOpenKeyA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Looking Glass", &hkeyLG) == ERROR_SUCCESS)
  {
    DWORD dataType;
    DWORD dataSize = sizeof(shmDevice);
    if (RegQueryValueExA(hkeyLG, "shmDevice", nullptr, &dataType, (BYTE*)&shmDevice, &dataSize) != ERROR_SUCCESS ||
        dataType != REG_DWORD)
      shmDevice = 0;
  }

  DWORD i = 0;
  for (auto it = m_devices.begin(); it != m_devices.end(); ++it, ++i)
  {
    DWORD bus = it->busAddr >> 32;
    DWORD addr = it->busAddr & 0xFFFFFFFF;
    DEBUG_INFO("IVSHMEM %u%c on bus 0x%lx, device 0x%lx, function 0x%lx",
      i, i == shmDevice ? '*' : ' ', bus, addr >> 16, addr & 0xFFFF);

    if (i == shmDevice)
      device = &(*it);
  }

  if (!device)
  {
    DEBUG_ERROR("Failed to match a shmDevice");
    SetupDiDestroyDeviceInfoList(devInfoSet);
    return false;
  }

  if (SetupDiEnumDeviceInterfaces(devInfoSet, &devInfoData, &GUID_DEVINTERFACE_IVSHMEM, 0, &devInterfaceData) == FALSE)
  {
    DEBUG_ERROR_HR(GetLastError(), "SetupDiEnumDeviceInterfaces");
    SetupDiDestroyDeviceInfoList(devInfoSet);
    return false;
  }

  DWORD reqSize = 0;
  SetupDiGetDeviceInterfaceDetail(devInfoSet, &devInterfaceData, nullptr, 0, &reqSize, nullptr);
  if (!reqSize)
  {
    DEBUG_ERROR_HR(GetLastError(), "SetupDiGetDeviceInterfaceDetail");
    SetupDiDestroyDeviceInfoList(devInfoSet);
    return false;
  }

  infData = (PSP_DEVICE_INTERFACE_DETAIL_DATA)calloc(1, reqSize);
  infData->cbSize = sizeof(PSP_DEVICE_INTERFACE_DETAIL_DATA);
  if (!SetupDiGetDeviceInterfaceDetail(devInfoSet, &devInterfaceData, infData, reqSize, nullptr, nullptr))
  {
    DEBUG_ERROR_HR(GetLastError(), "SetupDiGetDeviceInterfaceDetail");
    SetupDiDestroyDeviceInfoList(devInfoSet);
    return false;
  }

  m_handle = CreateFile(infData->DevicePath, 0, 0, nullptr, OPEN_EXISTING, 0, 0);
  if (m_handle == INVALID_HANDLE_VALUE)
  {
    DEBUG_ERROR_HR(GetLastError(), "CreateFile");
    SetupDiDestroyDeviceInfoList(devInfoSet);
    return false;
  }

  SetupDiDestroyDeviceInfoList(devInfoSet);
  DEBUG_TRACE("IVSHMEM Initialized");
  return true;
}

bool CIVSHMEM::Open()
{
  IVSHMEM_SIZE size;
  if (!DeviceIoControl(m_handle, IOCTL_IVSHMEM_REQUEST_SIZE, nullptr, 0, &size, sizeof(size), nullptr, nullptr))
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to request ivshmem size");
    return false;
  }

  IVSHMEM_MMAP_CONFIG config = {};
  IVSHMEM_MMAP map           = {};

  config.cacheMode = IVSHMEM_CACHE_WRITECOMBINED;
  if (!DeviceIoControl(m_handle, IOCTL_IVSHMEM_REQUEST_MMAP, &config, sizeof(config), &map, sizeof(map), nullptr, nullptr))
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to request ivshmem mmap");
    return false;
  }

  m_size = (size_t)size;
  m_mem  = map.ptr;

  return true;
}

void CIVSHMEM::Close()
{
  if (m_mem == nullptr)
    return;

  if (!DeviceIoControl(m_handle, IOCTL_IVSHMEM_RELEASE_MMAP, nullptr, 0, nullptr, 0, nullptr, nullptr))
  {
    DEBUG_ERROR("Failed to release ivshmem mmap");
    return;
  }

  m_size = 0;
  m_mem  = nullptr;
}