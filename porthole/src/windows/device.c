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

#include "porthole/device.h"
#include "driver.h"

#include "common/debug.h"

#include <windows.h>
#include <setupapi.h>

struct PortholeDev
{
  HANDLE dev;
};

bool porthole_dev_open(PortholeDev *handle, const uint32_t vendor_id)
{
  HDEVINFO                         devInfo    = {0};
  PSP_DEVICE_INTERFACE_DETAIL_DATA infData    = NULL;
  SP_DEVICE_INTERFACE_DATA         devInfData = {0};
  HANDLE                           dev;

  if (!handle)
  {
    DEBUG_ERROR("Invalid buffer provided");
    return false;
  }

  devInfo = SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES | DIGCF_DEVICEINTERFACE);
  devInfData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

  for(int devIndex = 0; ; ++devIndex)
  {
    if (SetupDiEnumDeviceInterfaces(devInfo, NULL, &GUID_DEVINTERFACE_PORTHOLE, devIndex, &devInfData) == FALSE)
    {
      DWORD error = GetLastError();
      if (error == ERROR_NO_MORE_ITEMS)
      {
        DEBUG_ERROR("Unable to enumerate the device, is it attached?");
        SetupDiDestroyDeviceInfoList(devInfo);
        return false;
      }
    }

    DWORD reqSize = 0;
    SetupDiGetDeviceInterfaceDetail(devInfo, &devInfData, NULL, 0, &reqSize, NULL);
    if (!reqSize)
    {
      DEBUG_WARN("SetupDiGetDeviceInterfaceDetail for %lu failed\n", reqSize);
      continue;
    }

    infData         = (PSP_DEVICE_INTERFACE_DETAIL_DATA)calloc(reqSize, 1);
    infData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
    if (!SetupDiGetDeviceInterfaceDetail(devInfo, &devInfData, infData, reqSize, NULL, NULL))
    {
      free(infData);
      DEBUG_WARN("SetupDiGetDeviceInterfaceDetail for %lu failed\n", reqSize);
      continue;
    }

    /* get the subsys id from the device */
    unsigned int vendorID, deviceID, subsysID;
    if (sscanf(infData->DevicePath, "\\\\?\\pci#ven_%4x&dev_%4x&subsys_%8x", &vendorID, &deviceID, &subsysID) != 3)
    {
      free(infData);
      DEBUG_ERROR("Failed to parse: %s", infData->DevicePath);
      continue;
    }

    if (subsysID != vendor_id)
    {
      DEBUG_INFO("Skipping device %d, vendor_id 0x%x != 0x%x", devIndex, subsysID, vendor_id);
      free(infData);
      continue;
    }

    dev = CreateFile(infData->DevicePath, 0, 0, NULL, OPEN_EXISTING, 0, 0);
    if (dev == INVALID_HANDLE_VALUE)
    {
      DEBUG_ERROR("Failed to open device %d", devIndex);
      free(infData);
      continue;
    }

    DEBUG_INFO("Device found");

    free(infData);
    break;
  }

  *handle = (PortholeDev)calloc(sizeof(struct PortholeDev), 1);
  if (!*handle)
  {
    DEBUG_ERROR("Failed to allocate PortholeDev struct, out of memory!");
    CloseHandle(dev);
    return false;
  }

  (*handle)->dev = dev;

  return true;
}

void porthole_dev_close(PortholeDev *handle)
{
  CloseHandle((*handle)->dev);
  free(*handle);
  *handle = NULL;
}

bool porthole_dev_share(PortholeDev handle, const uint32_t type, void *buffer, size_t size)
{
  DWORD returned;

	PortholeMsg msg = {
    .type = type,
    .addr = buffer,
    .size = size
  };

	if (!DeviceIoControl(handle->dev, IOCTL_PORTHOLE_SEND_MSG, &msg, sizeof(PortholeMsg), NULL, 0, &returned, NULL))
    return false;

  return true;
}

bool porthole_dev_unlock(PortholeDev handle, void *buffer, size_t size)
{
  DWORD returned;

	PortholeLockMsg msg = {
    .addr = buffer,
    .size = size
  };

	if (!DeviceIoControl(handle->dev, IOCTL_PORTHOLE_UNLOCK_BUFFER, &msg , sizeof(PortholeLockMsg), NULL, 0, &returned, NULL))
    return false;

  return true;
}