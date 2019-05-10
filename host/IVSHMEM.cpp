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

#include "IVSHMEM.h"

#include <windows.h>
#include <setupapi.h>
#include "kvm-guest-drivers-windows/ivshmem/Public.h"
#include "common/debug.h"

IVSHMEM * IVSHMEM::m_instance = NULL;

void IVSHMEM::listDevices() {
  
  HDEVINFO deviceInfoSet;
  PSP_DEVICE_INTERFACE_DETAIL_DATA infData = NULL;
  SP_DEVICE_INTERFACE_DATA deviceInterfaceData;

  deviceInfoSet = SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES | DIGCF_DEVICEINTERFACE);
  ZeroMemory(&deviceInterfaceData, sizeof(SP_DEVICE_INTERFACE_DATA));
  deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

  fprintf(stderr, "Found devices:\n");

  DWORD i = 0;
  while(SetupDiEnumDeviceInterfaces(deviceInfoSet, NULL, &GUID_DEVINTERFACE_IVSHMEM, i, &deviceInterfaceData) != FALSE)
  {
    DWORD reqSize = 0;
    SP_DEVINFO_DATA d;
    d.cbSize = sizeof(SP_DEVINFO_DATA);

    SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData, NULL, 0, &reqSize, &d);
    if (!reqSize)
    {
      fprintf(stderr, "SetupDiGetDeviceInterfaceDetail");
      break;
    }
    
    infData = static_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA>(malloc(reqSize));
    ZeroMemory(infData, reqSize);
    infData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
    if (!SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData, infData, reqSize, NULL, NULL))
    {
      fprintf(stderr, "SetupDiGetDeviceInterfaceDetail");
      break;
    }

    DWORD bus, addr, slot, func;

    if (!SetupDiGetDeviceRegistryProperty(deviceInfoSet, &d,SPDRP_BUSNUMBER,NULL,(PBYTE)&bus,sizeof(bus),NULL))
    {
      DEBUG_ERROR("SetupDiGetDeviceRegistryProperty - SPDRP_BUSNUMBER: %lu\n", GetLastError());
      free(infData);
      break;
    }

    if (!SetupDiGetDeviceRegistryProperty(deviceInfoSet, &d,SPDRP_ADDRESS,NULL, (PBYTE)&addr,sizeof(addr),NULL))
    {
      DEBUG_ERROR("SetupDiGetDeviceRegistryProperty - SPDRP_ADDRESS: %lu\n", GetLastError());
      free(infData);
      break;
    }
    
    slot = (addr >> 16) & 0xFFFF;
    func = addr & 0xFFFF;

    fprintf(stderr, "[%lu] Found Device: %ls\n"
               "   Bus:  0x%lx\n"
               "   Slot: 0x%lx\n"
               "   Func: 0x%lx\n", 
               i, infData->DevicePath, bus, slot, func);
    i++;
  }
  
  DWORD error = GetLastError();
  if (error != ERROR_NO_MORE_ITEMS)
  {
    fprintf(stderr, "Unknown error on index %u: %lu\n", i, error);
  }

  fprintf(stderr, "%lu devices found\n\n", i);
} 

IVSHMEM::IVSHMEM() :
  m_initialized(false),
  m_handle(INVALID_HANDLE_VALUE),
  m_gotSize(false),
  m_gotPeerID(false),
  m_gotMemory(false)
{

}

IVSHMEM::~IVSHMEM()
{
  DeInitialize();
}

bool IVSHMEM::Initialize(PCI_DEVICE dev)
{
  if (m_initialized)
    DeInitialize();

  HDEVINFO deviceInfoSet;
  PSP_DEVICE_INTERFACE_DETAIL_DATA infData = NULL;
  SP_DEVICE_INTERFACE_DATA deviceInterfaceData;

  deviceInfoSet = SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES | DIGCF_DEVICEINTERFACE);
  ZeroMemory(&deviceInterfaceData, sizeof(SP_DEVICE_INTERFACE_DATA));
  deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

  DWORD devid = 0;
  while (SetupDiEnumDeviceInterfaces(deviceInfoSet, NULL, &GUID_DEVINTERFACE_IVSHMEM, devid, &deviceInterfaceData) != FALSE)
  {
    DWORD reqSize = 0;
    SP_DEVINFO_DATA d;
    d.cbSize = sizeof(SP_DEVINFO_DATA);
    SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData, NULL, 0, &reqSize, &d);
    if (!reqSize)
    {
      DEBUG_ERROR("SetupDiGetDeviceInterfaceDetail");
      break;
    }

    infData = static_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA>(malloc(reqSize));
    ZeroMemory(infData, reqSize);
    infData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
    if (!SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData, infData, reqSize, NULL, NULL))
    {
      DEBUG_ERROR("SetupDiGetDeviceInterfaceDetail");
      free(infData);
      break;
    }

    DWORD bus, addr, slot, func;

    if (!SetupDiGetDeviceRegistryProperty(deviceInfoSet, &d,SPDRP_BUSNUMBER,NULL,(PBYTE)&bus,sizeof(bus),NULL))
    {
      DEBUG_ERROR("SetupDiGetDeviceRegistryProperty - SPDRP_BUSNUMBER");
      free(infData);
      break;
    }

    if (!SetupDiGetDeviceRegistryProperty(deviceInfoSet, &d,SPDRP_ADDRESS,NULL, (PBYTE)&addr,sizeof(addr),NULL))
    {
      DEBUG_ERROR("SetupDiGetDeviceRegistryProperty - SPDRP_ADDRESS");
      free(infData);
      break;
    }

    slot = (addr >> 16) & 0xFFFF;
    func = addr & 0xFFFF;

    if (dev.bus != bus || dev.addr != slot || dev.func != func) //not the right device, keep searching
    {
      free(infData);
      devid++;
      continue;
    }

    fprintf(stderr, "Found Device at Index %lu: %ls\n"
               "   Bus:  %lu\n"
               "   Addr: %lu\n"
               "   Func: %lu\n", 
               devid, infData->DevicePath, bus, slot, func);

    m_handle = CreateFile(infData->DevicePath, 0, 0, NULL, OPEN_EXISTING, 0, 0);
    if (m_handle == INVALID_HANDLE_VALUE)
    {
      DEBUG_ERROR("CreateFile returned INVALID_HANDLE_VALUE");
      free(infData);
      break;
    }

    m_initialized = true;
    break;
  }
  
  DWORD error = GetLastError();
  if (m_initialized == false)
  {
    if (error == ERROR_NO_MORE_ITEMS)
    {
      DEBUG_ERROR("Unable to enumerate the device, is it attached?");
    }
    else
    {
      DEBUG_ERROR("Unable to enumerate the device. Error: %lu\n", error);
    }
    
  } 

  SetupDiDestroyDeviceInfoList(deviceInfoSet);
  return m_initialized;
}

void IVSHMEM::DeInitialize()
{
  if (!m_initialized)
    return;

  if (m_gotMemory)
  {
    if (!DeviceIoControl(m_handle, IOCTL_IVSHMEM_RELEASE_MMAP, NULL, 0, NULL, 0, NULL, NULL))
      DEBUG_ERROR("DeviceIoControl failed: %d", (int)GetLastError());
    m_memory = NULL;
  }

  if (m_handle != INVALID_HANDLE_VALUE)
    CloseHandle(m_handle);

  m_initialized = false;
  m_handle      = INVALID_HANDLE_VALUE;
  m_gotSize     = false;
  m_gotPeerID   = false;
  m_gotVectors  = false;
  m_gotMemory   = false;
}

bool IVSHMEM::IsInitialized()
{
  return m_initialized;
}

UINT64 IVSHMEM::GetSize()
{
  if (!m_initialized)
    return 0;

  if (m_gotSize)
    return m_size;

  IVSHMEM_SIZE size;
  if (!DeviceIoControl(m_handle, IOCTL_IVSHMEM_REQUEST_SIZE, NULL, 0, &size, sizeof(IVSHMEM_SIZE), NULL, NULL))
  {
    DEBUG_ERROR("DeviceIoControl Failed: %d", (int)GetLastError());
    return 0;
  }

  m_gotSize = true;
  m_size    = static_cast<UINT64>(size);
  return m_size;
}

UINT16 IVSHMEM::GetPeerID()
{
  if (!m_initialized)
    return 0;

  if (m_gotPeerID)
    return m_peerID;

  IVSHMEM_PEERID peerID;
  if (!DeviceIoControl(m_handle, IOCTL_IVSHMEM_REQUEST_SIZE, NULL, 0, &peerID, sizeof(IVSHMEM_PEERID), NULL, NULL))
  {
    DEBUG_ERROR("DeviceIoControl Failed: %d", (int)GetLastError());
    return 0;
  }

  m_gotPeerID = true;
  m_peerID    = static_cast<UINT16>(peerID);
  return m_peerID;
}

UINT16 IVSHMEM::GetVectors()
{
  if (!m_initialized)
    return 0;

  if (!m_gotVectors)
    return 0;

  return m_vectors;
}

void * IVSHMEM::GetMemory()
{
  if (!m_initialized)
    return NULL;

  if (m_gotMemory)
    return m_memory;

// this if define can be removed later once everyone is un the latest version
// old versions of the IVSHMEM driver ignore the input argument, as such this
// is completely backwards compatible
#if defined(IVSHMEM_CACHE_WRITECOMBINED)
  IVSHMEM_MMAP_CONFIG config;
  config.cacheMode = IVSHMEM_CACHE_WRITECOMBINED;
#endif

  IVSHMEM_MMAP map;
  ZeroMemory(&map, sizeof(IVSHMEM_MMAP));
  if (!DeviceIoControl(
    m_handle,
    IOCTL_IVSHMEM_REQUEST_MMAP,
#if defined(IVSHMEM_CACHE_WRITECOMBINED)
    &config, sizeof(IVSHMEM_MMAP_CONFIG),
#else
    NULL   , 0,
#endif
    &map   , sizeof(IVSHMEM_MMAP       ),
    NULL, NULL))
  {
    DEBUG_ERROR("DeviceIoControl Failed: %d", (int)GetLastError());
    return NULL;
  }

  m_gotSize    = true;
  m_gotPeerID  = true;
  m_gotMemory  = true;
  m_gotVectors = true;
  m_size       = static_cast<UINT64>(map.size   );
  m_peerID     = static_cast<UINT16>(map.peerID );
  m_vectors    = static_cast<UINT16>(map.vectors);
  m_memory     = map.ptr;

  return m_memory;
}

HANDLE IVSHMEM::CreateVectorEvent(UINT16 vector)
{
  if (!m_initialized)
    return INVALID_HANDLE_VALUE;

  HANDLE event = CreateEvent(NULL, TRUE, FALSE, NULL);
  if (event == INVALID_HANDLE_VALUE)
  {
    DEBUG_ERROR("CreateEvent Failed: %d", (int)GetLastError());
    return INVALID_HANDLE_VALUE;
  }

  IVSHMEM_EVENT msg;
  msg.event      = event;
  msg.singleShot = false;
  msg.vector     = vector;

  if (!DeviceIoControl(m_handle, IOCTL_IVSHMEM_REGISTER_EVENT, &msg, sizeof(IVSHMEM_EVENT), NULL, 0, NULL, NULL))
  {
    DEBUG_ERROR("DeviceIoControl Failed: %d", (int)GetLastError());
    CloseHandle(event);
    return INVALID_HANDLE_VALUE;
  }

  return event;
}

bool IVSHMEM::RingDoorbell(UINT16 peerID, UINT16 door)
{
  if (!m_initialized)
    return false;

  IVSHMEM_RING msg;
  msg.peerID = peerID;
  msg.vector = door;

  if (!DeviceIoControl(m_handle, IOCTL_IVSHMEM_RING_DOORBELL, &msg, sizeof(IVSHMEM_RING), NULL, 0, NULL, NULL))
  {
    DEBUG_ERROR("DeviceIoControl Failed: %d", (int)GetLastError());
    return false;
  }

  return true;
}