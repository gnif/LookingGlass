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
#include <assert.h>

struct PortholeDev
{
  HANDLE         dev;
  bool           connected;
  PortholeEvents events;
};

bool porthole_dev_open(PortholeDev *handle, const uint32_t vendor_id)
{
  HDEVINFO                         devInfo    = {0};
  PSP_DEVICE_INTERFACE_DETAIL_DATA infData    = NULL;
  SP_DEVICE_INTERFACE_DATA         devInfData = {0};
  HANDLE                           dev;

  assert(handle);

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

  /* create the events and register them */
  (*handle)->events.connect    = CreateEvent(NULL, FALSE, FALSE, NULL);
  (*handle)->events.disconnect = CreateEvent(NULL, FALSE, FALSE, NULL);

  DWORD returned;
  if (!DeviceIoControl(dev, IOCTL_PORTHOLE_REGISTER_EVENTS, &(*handle)->events, sizeof(PortholeEvents), NULL, 0, &returned, NULL))
  {
    DEBUG_ERROR("Failed to register the events");
    CloseHandle((*handle)->events.connect   );
    CloseHandle((*handle)->events.disconnect);
    CloseHandle(dev);
    free(*handle);
    *handle = NULL;
    return false;
  }

  return true;
}

void porthole_dev_close(PortholeDev *handle)
{
  assert(handle && *handle);

  CloseHandle((*handle)->events.connect   );
  CloseHandle((*handle)->events.disconnect);
  CloseHandle((*handle)->dev);
  free(*handle);
  *handle = NULL;
}

static PortholeState get_state(PortholeDev handle, unsigned int timeout)
{
  if (handle->connected)
  {
    switch(WaitForSingleObject(handle->events.disconnect, timeout))
    {
      case WAIT_OBJECT_0:
        handle->connected = false;
        return PH_STATE_DISCONNECTED;

      case WAIT_TIMEOUT:
        return PH_STATE_CONNECTED;

      default:
        DEBUG_FATAL("Error waiting on disconnect event");
        break;
    }
  }

  switch(WaitForSingleObject(handle->events.connect, timeout))
  {
    case WAIT_OBJECT_0:
      handle->connected = true;
      return PH_STATE_NEW_SESSION;

    case WAIT_TIMEOUT:
      return PH_STATE_DISCONNECTED;

    default:
      DEBUG_FATAL("Error waiting on connection event");
      break;
  }
}

PortholeState porthole_dev_get_state(PortholeDev handle)
{
  return get_state(handle, 0);
}

bool porthole_dev_wait_state(PortholeDev handle, const PortholeState state, const unsigned int timeout)
{
  const DWORD to = (timeout == 0) ? INFINITE : timeout;
  PortholeState lastState = get_state(handle, 0);

  if (state == lastState)
    return true;

  while(true)
  {
    PortholeState nextState;
    switch(lastState)
    {
      case PH_STATE_DISCONNECTED:
        nextState = PH_STATE_NEW_SESSION;
        break;

      case PH_STATE_NEW_SESSION:
        nextState = PH_STATE_CONNECTED;
        break;

      case PH_STATE_CONNECTED:
        nextState = PH_STATE_DISCONNECTED;
        break;
    }

    PortholeState newState = get_state(handle, to);
    if (newState == lastState || newState != nextState)
      return false;

    if (newState == state)
      return true;

    lastState = newState;
  }
}

PortholeID porthole_dev_map(PortholeDev handle, const uint32_t type, void *buffer, size_t size)
{
  assert(handle);

  DWORD returned;

  PortholeMsg msg = {
    .type = type,
    .addr = buffer,
    .size = size
  };

  PortholeMapID out;

  if (!DeviceIoControl(handle->dev, IOCTL_PORTHOLE_SEND_MSG, &msg, sizeof(PortholeMsg), &out, sizeof(PortholeMapID), &returned, NULL))
    return -1;

  PortholeID ret = out;
  return ret;
}

bool porthole_dev_unmap(PortholeDev handle, PortholeID id)
{
  assert(handle);

  DWORD returned;

  PortholeMapID msg = id;
  if (!DeviceIoControl(handle->dev, IOCTL_PORTHOLE_UNLOCK_BUFFER, &msg, sizeof(PortholeMapID), NULL, 0, &returned, NULL))
    return false;

  return true;
}