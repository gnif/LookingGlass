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

#include "common/ivshmem.h"
#include "common/option.h"
#include "common/vector.h"
#include "common/windebug.h"

#include <windows.h>
#include "ivshmem.h"

#include <setupapi.h>
#include <stdlib.h>
#include <string.h>

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

static bool ivshmemCacheModeToDriver(IVSHMEMCacheMode cacheMode,
    UINT8 * driverMode)
{
  switch (cacheMode)
  {
    case IVSHMEM_CACHE_MODE_NONCACHED:
      *driverMode = IVSHMEM_CACHE_NONCACHED;
      return true;

    case IVSHMEM_CACHE_MODE_CACHED:
      *driverMode = IVSHMEM_CACHE_CACHED;
      return true;

    case IVSHMEM_CACHE_MODE_WRITECOMBINED:
      *driverMode = IVSHMEM_CACHE_WRITECOMBINED;
      return true;
  }

  DEBUG_ERROR("Invalid IVSHMEM cache mode: %d", (int)cacheMode);
  return false;
}

bool ivshmemInit(struct IVSHMEM * dev)
{
  DEBUG_ASSERT(dev && !dev->opaque && !dev->mem);

  dev->size   = 0;
  dev->mem    = NULL;
  dev->opaque = NULL;

  HANDLE                           handle           = INVALID_HANDLE_VALUE;
  HDEVINFO                         devInfoSet       = INVALID_HANDLE_VALUE;
  PSP_DEVICE_INTERFACE_DETAIL_DATA infData          = NULL;
  SP_DEVINFO_DATA                  devInfoData       = {0};
  SP_DEVICE_INTERFACE_DATA         devInterfaceData = {0};
  Vector                           devices;

  devInfoSet = SetupDiGetClassDevs(&GUID_DEVINTERFACE_IVSHMEM, NULL, NULL,
      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (devInfoSet == INVALID_HANDLE_VALUE)
  {
    DEBUG_WINERROR("SetupDiGetClassDevs failed", GetLastError());
    return false;
  }

  devInfoData.cbSize      = sizeof(SP_DEVINFO_DATA);
  devInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

  if (!vector_create(&devices, sizeof(struct IVSHMEMData), 1))
  {
    SetupDiDestroyDeviceInfoList(devInfoSet);
    DEBUG_ERROR("Failed to allocate memory");
    return false;
  }

  for (int i = 0; SetupDiEnumDeviceInfo(devInfoSet, i, &devInfoData); ++i)
  {
    struct IVSHMEMData * device = vector_push(&devices, NULL);
    if (!device)
    {
      vector_destroy(&devices);
      SetupDiDestroyDeviceInfoList(devInfoSet);
      return false;
    }

    DWORD bus;
    DWORD addr;
    if (!SetupDiGetDeviceRegistryProperty(devInfoSet, &devInfoData,
          SPDRP_BUSNUMBER, NULL, (void*) &bus, sizeof(bus), NULL))
    {
      DEBUG_WINERROR("Failed to SetupDiGetDeviceRegistryProperty",
          GetLastError());
      bus = 0xFFFF;
    }

    if (!SetupDiGetDeviceRegistryProperty(devInfoSet, &devInfoData,
          SPDRP_ADDRESS, NULL, (void*) &addr, sizeof(addr), NULL))
    {
      DEBUG_WINERROR("Failed to SetupDiGetDeviceRegistryProperty",
          GetLastError());
      addr = 0xFFFFFFFF;
    }

    device->busAddr = (((DWORD64) bus) << 32) | addr;
    memcpy(&device->devInfoData, &devInfoData, sizeof(SP_DEVINFO_DATA));
  }

  const DWORD enumError = GetLastError();
  if (enumError != ERROR_NO_MORE_ITEMS)
  {
    vector_destroy(&devices);
    SetupDiDestroyDeviceInfoList(devInfoSet);
    DEBUG_WINERROR("SetupDiEnumDeviceInfo failed", enumError);
    return false;
  }

  if (vector_size(&devices) == 0)
  {
    vector_destroy(&devices);
    SetupDiDestroyDeviceInfoList(devInfoSet);
    DEBUG_ERROR("Failed to find any IVSHMEM devices, unable to continue");
    DEBUG_ERROR("Did you remember to add the device to your VM "
        "and is the driver installed?");
    return false;
  }

  const int shmDevice = option_get_int("os", "shmDevice");
  qsort(vector_data(&devices), vector_size(&devices), sizeof(struct IVSHMEMData),
      ivshmemComparator);

  struct IVSHMEMData * device;
  vector_forEachRefIdx(i, device, &devices)
  {
    DWORD bus  = device->busAddr >> 32;
    DWORD addr = device->busAddr & 0xFFFFFFFF;
    DEBUG_INFO(
      "IVSHMEM %" PRIuPTR "%c on bus 0x%lx, device 0x%lx, function 0x%lx",
      i,
      i == (uintptr_t)shmDevice ? '*' : ' ',
      bus,
      addr >> 16,
      addr & 0xFFFF);
  }

  if (shmDevice < 0 || (uintptr_t)shmDevice >= vector_size(&devices))
  {
    vector_destroy(&devices);
    SetupDiDestroyDeviceInfoList(devInfoSet);
    DEBUG_ERROR("os:shmDevice %d does not exist", shmDevice);
    return false;
  }

  device = vector_ptrTo(&devices, shmDevice);
  memcpy(&devInfoData, &device->devInfoData, sizeof(SP_DEVINFO_DATA));
  vector_destroy(&devices);

  if (SetupDiEnumDeviceInterfaces(devInfoSet, &devInfoData,
        &GUID_DEVINTERFACE_IVSHMEM, 0, &devInterfaceData) == FALSE)
  {
    const DWORD error = GetLastError();
    SetupDiDestroyDeviceInfoList(devInfoSet);
    DEBUG_WINERROR("SetupDiEnumDeviceInterfaces failed", error);
    return false;
  }

  DWORD reqSize = 0;
  SetupDiGetDeviceInterfaceDetail(devInfoSet, &devInterfaceData,
      NULL, 0, &reqSize, NULL);
  if (!reqSize)
  {
    const DWORD error = GetLastError();
    SetupDiDestroyDeviceInfoList(devInfoSet);
    DEBUG_WINERROR("SetupDiGetDeviceInterfaceDetail", error);
    return false;
  }

  infData         = calloc(1, reqSize);
  if (!infData)
  {
    SetupDiDestroyDeviceInfoList(devInfoSet);
    DEBUG_ERROR("Failed to allocate IVSHMEM interface data");
    return false;
  }
  infData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
  if (!SetupDiGetDeviceInterfaceDetail(devInfoSet, &devInterfaceData, infData,
        reqSize, NULL, NULL))
  {
    const DWORD error = GetLastError();
    free(infData);
    SetupDiDestroyDeviceInfoList(devInfoSet);
    DEBUG_WINERROR("SetupDiGetDeviceInterfaceDetail", error);
    return false;
  }

  handle = CreateFile(infData->DevicePath, 0, 0, NULL, OPEN_EXISTING, 0, 0);
  if (handle == INVALID_HANDLE_VALUE)
  {
    const DWORD error = GetLastError();
    free(infData);
    SetupDiDestroyDeviceInfoList(devInfoSet);
    DEBUG_WINERROR("CreateFile returned INVALID_HANDLE_VALUE", error);
    return false;
  }

  free(infData);
  SetupDiDestroyDeviceInfoList(devInfoSet);

  struct IVSHMEMInfo * info = malloc(sizeof(*info));
  if (!info)
  {
    CloseHandle(handle);
    DEBUG_ERROR("Failed to allocate IVSHMEM device state");
    return false;
  }

  info->handle = handle;
  dev->opaque  = info;
  dev->size    = 0;
  dev->mem     = NULL;

  return true;
}

bool ivshmemOpen(struct IVSHMEM * dev, IVSHMEMCacheMode cacheMode)
{
  DEBUG_ASSERT(dev && dev->opaque && !dev->mem);

  struct IVSHMEMInfo * info = (struct IVSHMEMInfo *)dev->opaque;

  UINT8 driverMode;
  if (!ivshmemCacheModeToDriver(cacheMode, &driverMode))
    return false;

  IVSHMEM_SIZE size;
  if (!DeviceIoControl(info->handle, IOCTL_IVSHMEM_REQUEST_SIZE, NULL, 0, &size,
        sizeof(IVSHMEM_SIZE), NULL, NULL))
  {
    DEBUG_WINERROR("DeviceIoControl Failed", GetLastError());
    return false;
  }

  const size_t mapSize = (size_t)size;
  if (!mapSize || (IVSHMEM_SIZE)mapSize != size)
  {
    DEBUG_ERROR("Invalid IVSHMEM size: %" PRIu64, (uint64_t)size);
    return false;
  }

  IVSHMEM_MMAP_CONFIG config = { .cacheMode = driverMode };
  IVSHMEM_MMAP        map    = { 0 };
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

  if (!map.ptr || map.size != size)
  {
    DEBUG_ERROR("IVSHMEM driver returned an invalid mapping");
    DeviceIoControl(info->handle, IOCTL_IVSHMEM_RELEASE_MMAP, NULL, 0, NULL,
        0, NULL, NULL);
    return false;
  }

  dev->size = mapSize;
  dev->mem  = map.ptr;
  return true;
}

bool ivshmemOpenDev(struct IVSHMEM * dev, const char * shmDev,
    IVSHMEMCacheMode cacheMode)
{
  DEBUG_ASSERT(dev);
  (void)shmDev;
  (void)cacheMode;

  DEBUG_ERROR("Opening an IVSHMEM device by path is not supported on Windows");
  return false;
}

void ivshmemClose(struct IVSHMEM * dev)
{
  DEBUG_ASSERT(dev);

  if (!dev->opaque || !dev->mem)
    return;

  struct IVSHMEMInfo * info = (struct IVSHMEMInfo *)dev->opaque;

  if (!DeviceIoControl(info->handle, IOCTL_IVSHMEM_RELEASE_MMAP, NULL, 0, NULL,
        0, NULL, NULL))
    DEBUG_WINERROR("DeviceIoControl failed", GetLastError());

  dev->size = 0;
  dev->mem  = NULL;
}

void ivshmemFree(struct IVSHMEM * dev)
{
  DEBUG_ASSERT(dev);

  if (!dev->opaque)
    return;

  if (dev->mem)
    ivshmemClose(dev);

  struct IVSHMEMInfo * info = (struct IVSHMEMInfo *)dev->opaque;

  CloseHandle(info->handle);
  free(info);
  dev->opaque = NULL;
}
