/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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

#include "common/ivshmem.h"
#include "common/option.h"
#include "common/windebug.h"

#include <windows.h>
#include "ivshmem.h"

#include <assert.h>
#include <setupapi.h>
#include <io.h>

struct IVSHMEMInfo
{
  HANDLE handle;
};

void ivshmemOptionsInit(void)
{
  static struct Option options[] = {
    {
      .module         = "os",
      .name           = "shmDevice",
      .description    = "The IVSHMEM device to use",
      .type           = OPTION_TYPE_INT,
      .value.x_int    = 0
    },
    {0}
  };

  option_register(options);
}

bool ivshmemInit(struct IVSHMEM * dev)
{
  assert(dev && !dev->opaque);

  HANDLE                           handle;
  HDEVINFO                         devInfoSet;
  PSP_DEVICE_INTERFACE_DETAIL_DATA infData = NULL;
  SP_DEVICE_INTERFACE_DATA         devInterfaceData = {0};

  devInfoSet = SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES | DIGCF_DEVICEINTERFACE);
  devInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

  const int shmDevice = option_get_int("os", "shmDevice");
  if (SetupDiEnumDeviceInterfaces(devInfoSet, NULL, &GUID_DEVINTERFACE_IVSHMEM, shmDevice, &devInterfaceData) == FALSE)
  {
    DWORD error = GetLastError();
    if (error == ERROR_NO_MORE_ITEMS)
    {
      DEBUG_WINERROR("Unable to enumerate the device, is it attached?", error);
      return false;
    }

    DEBUG_WINERROR("SetupDiEnumDeviceInterfaces failed", error);
    return false;
  }

  DWORD reqSize = 0;
  SetupDiGetDeviceInterfaceDetail(devInfoSet, &devInterfaceData, NULL, 0, &reqSize, NULL);
  if (!reqSize)
  {
    DEBUG_WINERROR("SetupDiGetDeviceInterfaceDetail", GetLastError());
    return false;
  }

  infData         = (PSP_DEVICE_INTERFACE_DETAIL_DATA)calloc(reqSize, 1);
  infData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
  if (!SetupDiGetDeviceInterfaceDetail(devInfoSet, &devInterfaceData, infData, reqSize, NULL, NULL))
  {
    free(infData);
    DEBUG_WINERROR("SetupDiGetDeviceInterfaceDetail", GetLastError());
    return false;
  }

  handle = CreateFile(infData->DevicePath, 0, 0, NULL, OPEN_EXISTING, 0, 0);
  if (handle == INVALID_HANDLE_VALUE)
  {
    SetupDiDestroyDeviceInfoList(devInfoSet);
    free(infData);
    DEBUG_WINERROR("CreateFile returned INVALID_HANDLE_VALUE", GetLastError());
    return false;
  }

  free(infData);
  SetupDiDestroyDeviceInfoList(devInfoSet);

  struct IVSHMEMInfo * info =
    (struct IVSHMEMInfo *)malloc(sizeof(struct IVSHMEMInfo));

  info->handle = handle;
  dev->opaque  = info;
  dev->size    = 0;
  dev->mem     = NULL;

  return true;
}

bool ivshmemOpen(struct IVSHMEM * dev)
{
  assert(dev && dev->opaque && !dev->mem);

  struct IVSHMEMInfo * info = (struct IVSHMEMInfo *)dev->opaque;

  IVSHMEM_SIZE size;
  if (!DeviceIoControl(info->handle, IOCTL_IVSHMEM_REQUEST_SIZE, NULL, 0, &size, sizeof(IVSHMEM_SIZE), NULL, NULL))
  {
    DEBUG_WINERROR("DeviceIoControl Failed", GetLastError());
    return 0;
  }

  IVSHMEM_MMAP_CONFIG config = { .cacheMode = IVSHMEM_CACHE_WRITECOMBINED };
  IVSHMEM_MMAP map = { 0 };
  if (!DeviceIoControl(
    info->handle,
    IOCTL_IVSHMEM_REQUEST_MMAP,
    &config, sizeof(IVSHMEM_MMAP_CONFIG),
    &map   , sizeof(IVSHMEM_MMAP),
    NULL, NULL))
  {
    DEBUG_WINERROR("DeviceIoControl Failed", GetLastError());
    return false;
  }

  dev->size   = (unsigned int)size;
  dev->mem    = map.ptr;
  return true;
}

void ivshmemClose(struct IVSHMEM * dev)
{
  assert(dev && dev->opaque && dev->mem);

  struct IVSHMEMInfo * info = (struct IVSHMEMInfo *)dev->opaque;

  if (!DeviceIoControl(info->handle, IOCTL_IVSHMEM_RELEASE_MMAP, NULL, 0, NULL, 0, NULL, NULL))
    DEBUG_WINERROR("DeviceIoControl failed", GetLastError());

  dev->size = 0;
  dev->mem  = NULL;
}

void ivshmemFree(struct IVSHMEM * dev)
{
  assert(dev && dev->opaque && !dev->mem);

  struct IVSHMEMInfo * info = (struct IVSHMEMInfo *)dev->opaque;

  free(info);
  dev->opaque = NULL;
}
