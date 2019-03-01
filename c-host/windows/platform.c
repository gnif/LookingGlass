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
#include <setupapi.h>

#include "app.h"
#include "debug.h"
#include "windebug.h"
#include "ivshmem/Public.h"

static HANDLE       shmemHandle = INVALID_HANDLE_VALUE;
static bool         shmemOwned  = false;
static IVSHMEM_MMAP shmemMap    = {0};

struct osThreadHandle
{
  const char       * name;
  osThreadFunction   function;
  void             * opaque;
  HANDLE             handle;
  DWORD              threadID;
  int                resultCode;
};

int WINAPI WinMain(HINSTANCE hInstnace, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
  HDEVINFO                         deviceInfoSet;
  PSP_DEVICE_INTERFACE_DETAIL_DATA infData = NULL;
  SP_DEVICE_INTERFACE_DATA         deviceInterfaceData;

  deviceInfoSet = SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES | DIGCF_DEVICEINTERFACE);
  memset(&deviceInterfaceData, 0, sizeof(SP_DEVICE_INTERFACE_DATA));
  deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

  if (SetupDiEnumDeviceInterfaces(deviceInfoSet, NULL, &GUID_DEVINTERFACE_IVSHMEM, 0, &deviceInterfaceData) == FALSE)
  {
    DWORD error = GetLastError();
    if (error == ERROR_NO_MORE_ITEMS)
    {
      DEBUG_WINERROR("Unable to enumerate the device, is it attached?", error);
      return -1;
    }

    DEBUG_WINERROR("SetupDiEnumDeviceInterfaces failed", error);
    return -1;
  }

  DWORD reqSize = 0;
  SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData, NULL, 0, &reqSize, NULL);
  if (!reqSize)
  {
    DEBUG_WINERROR("SetupDiGetDeviceInterfaceDetail", GetLastError());
    return -1;
  }

  infData         = (PSP_DEVICE_INTERFACE_DETAIL_DATA)calloc(reqSize, 1);
  infData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
  if (!SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData, infData, reqSize, NULL, NULL))
  {
    free(infData);
    DEBUG_WINERROR("SetupDiGetDeviceInterfaceDetail", GetLastError());
    return -1;
  }

  shmemHandle = CreateFile(infData->DevicePath, 0, 0, NULL, OPEN_EXISTING, 0, 0);
  if (shmemHandle == INVALID_HANDLE_VALUE)
  {
    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    free(infData);
    DEBUG_WINERROR("CreateFile returned INVALID_HANDLE_VALUE", GetLastError());
    return -1;
  }

  free(infData);
  SetupDiDestroyDeviceInfoList(deviceInfoSet);

  int result = app_main();

  os_shmemUnmap();
  CloseHandle(shmemHandle);

  if (result != 0)
  {
    MessageBoxA(
      NULL,
      "The Looking Glass host has terminated due to an error.\r\n"
      "\r\n"
      "For more information run the application in a command prompt.",
      "Looking Glass Host",
      MB_ICONERROR);
  }

  return result;
}

unsigned int os_shmemSize()
{
  IVSHMEM_SIZE size;
  if (!DeviceIoControl(shmemHandle, IOCTL_IVSHMEM_REQUEST_SIZE, NULL, 0, &size, sizeof(IVSHMEM_SIZE), NULL, NULL))
  {
    DEBUG_WINERROR("DeviceIoControl Failed", GetLastError());
    return 0;
  }

  return (unsigned int)size;
}

bool os_shmemMmap(void **ptr)
{
  if (shmemOwned)
  {
    *ptr = shmemMap.ptr;
    return true;
  }

  memset(&shmemMap, 0, sizeof(IVSHMEM_MMAP));
  if (!DeviceIoControl(
    shmemHandle,
    IOCTL_IVSHMEM_REQUEST_MMAP,
    NULL, 0,
    &shmemMap, sizeof(IVSHMEM_MMAP),
    NULL, NULL))
  {
    DEBUG_WINERROR("DeviceIoControl Failed", GetLastError());
    return false;
  }

  *ptr = shmemMap.ptr;
  shmemOwned = true;
  return true;
}

void os_shmemUnmap()
{
  if (!shmemOwned)
    return;

  if (!DeviceIoControl(shmemHandle, IOCTL_IVSHMEM_RELEASE_MMAP, NULL, 0, NULL, 0, NULL, NULL))
    DEBUG_WINERROR("DeviceIoControl failed", GetLastError());
  else
    shmemOwned = false;
}

static DWORD WINAPI threadWrapper(LPVOID lpParameter)
{
  osThreadHandle * handle = (osThreadHandle *)lpParameter;
  handle->resultCode = handle->function(handle->opaque);
  return 0;
}

bool os_createThread(const char * name, osThreadFunction function, void * opaque, osThreadHandle ** handle)
{
  *handle           = (osThreadHandle *)malloc(sizeof(osThreadHandle));
  (*handle)->name   = name;
  (*handle)->opaque = opaque;
  (*handle)->handle = CreateThread(NULL, 0, threadWrapper, *handle, 0, &(*handle)->threadID);

  if (!(*handle)->handle)
  {
    free(*handle);
    *handle = NULL;
    DEBUG_WINERROR("CreateThread failed", GetLastError());
    return false;
  }

  return true;
}

bool os_joinThread(osThreadHandle * handle, int * resultCode)
{
  while(true)
  {
    switch(WaitForSingleObject(handle->handle, INFINITE))
    {
      case WAIT_OBJECT_0:
        if (resultCode)
          *resultCode = handle->resultCode;
        CloseHandle(handle->handle);
        free(handle);
        return true;

      case WAIT_ABANDONED:
      case WAIT_TIMEOUT:
        continue;

      case WAIT_FAILED:
        DEBUG_WINERROR("Wait for thread failed", GetLastError());
        CloseHandle(handle->handle);
        free(handle);
        return false;
    }

    break;
  }

  DEBUG_WINERROR("Unknown failure waiting for thread", GetLastError());
  return false;
}