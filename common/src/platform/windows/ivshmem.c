/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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
#include "common/vector.h"
#include "common/windebug.h"

#include <windows.h>
#include "ivshmem.h"

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

struct IVSHMEMData
{
  SP_DEVINFO_DATA devInfoData;
  DWORD64         busAddr;
};

static int ivshmemComparator(const void * a_, const void * b_)
{
  const struct IVSHMEMData * a = a_;
  const struct IVSHMEMData * b = b_;

  if (a->busAddr < b->busAddr)
    return -1;

  if (a->busAddr > b->busAddr)
    return 1;

  return 0;
}

bool ivshmemInit(struct IVSHMEM * dev)
{
  DEBUG_ASSERT(dev && !dev->opaque);

  HANDLE                           handle;
  HDEVINFO                         devInfoSet;
  PSP_DEVICE_INTERFACE_DETAIL_DATA infData = NULL;
  SP_DEVINFO_DATA                  devInfoData = {0};
  SP_DEVICE_INTERFACE_DATA         devInterfaceData = {0};
  Vector                           devices;

  devInfoSet = SetupDiGetClassDevs(&GUID_DEVINTERFACE_IVSHMEM, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  devInfoData.cbSize      = sizeof(SP_DEVINFO_DATA);
  devInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

  if (!vector_create(&devices, sizeof(struct IVSHMEMData), 1))
  {
    DEBUG_ERROR("Failed to allocate memory");
    return false;
  }

  for (int i = 0; SetupDiEnumDeviceInfo(devInfoSet, i, &devInfoData); ++i)
  {
    struct IVSHMEMData * device = vector_push(&devices, NULL);

    DWORD bus, addr;
    if (!SetupDiGetDeviceRegistryProperty(devInfoSet, &devInfoData, SPDRP_BUSNUMBER,
        NULL, (void*) &bus, sizeof(bus), NULL))
    {
      DEBUG_WINERROR("Failed to SetupDiGetDeviceRegistryProperty", GetLastError());
      bus = 0xFFFF;
    }

    if (!SetupDiGetDeviceRegistryProperty(devInfoSet, &devInfoData, SPDRP_ADDRESS,
        NULL, (void*) &addr, sizeof(addr), NULL))
    {
      DEBUG_WINERROR("Failed to SetupDiGetDeviceRegistryProperty", GetLastError());
      addr = 0xFFFFFFFF;
    }

    device->busAddr = (((DWORD64) bus) << 32) | addr;
    memcpy(&device->devInfoData, &devInfoData, sizeof(SP_DEVINFO_DATA));
  }

  if (GetLastError() != ERROR_NO_MORE_ITEMS)
  {
    DEBUG_WINERROR("SetupDiEnumDeviceInfo failed", GetLastError());
    return false;
  }

  const int shmDevice = option_get_int("os", "shmDevice");
  qsort(vector_data(&devices), vector_size(&devices), sizeof(struct IVSHMEMData), ivshmemComparator);

  struct IVSHMEMData * device;
  vector_forEachRefIdx(i, device, &devices)
  {
    DWORD bus = device->busAddr >> 32;
    DWORD addr = device->busAddr & 0xFFFFFFFF;
    DEBUG_INFO("IVSHMEM %" PRIuPTR "%c on bus 0x%lx, device 0x%lx, function 0x%lx", i,
      i == shmDevice ? '*' : ' ', bus, addr >> 16, addr & 0xFFFF);
  }

  device = vector_ptrTo(&devices, shmDevice);
  memcpy(&devInfoData, &device->devInfoData, sizeof(SP_DEVINFO_DATA));
  vector_destroy(&devices);

  if (SetupDiEnumDeviceInterfaces(devInfoSet, &devInfoData, &GUID_DEVINTERFACE_IVSHMEM, 0, &devInterfaceData) == FALSE)
  {
    DEBUG_WINERROR("SetupDiEnumDeviceInterfaces failed", GetLastError());
    return false;
  }

  DWORD reqSize = 0;
  SetupDiGetDeviceInterfaceDetail(devInfoSet, &devInterfaceData, NULL, 0, &reqSize, NULL);
  if (!reqSize)
  {
    DEBUG_WINERROR("SetupDiGetDeviceInterfaceDetail", GetLastError());
    return false;
  }

  infData         = calloc(1, reqSize);
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

  struct IVSHMEMInfo * info = malloc(sizeof(*info));

  info->handle = handle;
  dev->opaque  = info;
  dev->size    = 0;
  dev->mem     = NULL;

  return true;
}

bool ivshmemOpen(struct IVSHMEM * dev)
{
  DEBUG_ASSERT(dev && dev->opaque && !dev->mem);

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
  DEBUG_ASSERT(dev && dev->opaque && dev->mem);

  struct IVSHMEMInfo * info = (struct IVSHMEMInfo *)dev->opaque;

  if (!DeviceIoControl(info->handle, IOCTL_IVSHMEM_RELEASE_MMAP, NULL, 0, NULL, 0, NULL, NULL))
    DEBUG_WINERROR("DeviceIoControl failed", GetLastError());

  dev->size = 0;
  dev->mem  = NULL;
}

void ivshmemFree(struct IVSHMEM * dev)
{
  DEBUG_ASSERT(dev && dev->opaque && !dev->mem);

  struct IVSHMEMInfo * info = (struct IVSHMEMInfo *)dev->opaque;

  free(info);
  dev->opaque = NULL;
}
