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

#include "Common.h"
#include "Adapter.h"
#include "BrokerDevice.h"

extern "C" DRIVER_INITIALIZE DriverEntry;

namespace
{
PDRIVER_UNLOAD g_portClsUnload = nullptr;

NTSTATUS AddAudioDevice(
  _In_ PDRIVER_OBJECT driverObject,
  _In_ PDEVICE_OBJECT physicalDeviceObject)
{
  constexpr ULONG kSubdeviceCount = 4;
  const NTSTATUS status = PcAddAdapterDevice(driverObject,
    physicalDeviceObject, StartAudioDevice, kSubdeviceCount,
    kLGAudioPortClsDeviceExtensionSize);
  if (NT_SUCCESS(status))
    BrokerDeviceNotifyPnpDeviceAdded();
  return status;
}

NTSTATUS DispatchIrp(
  _In_ PDEVICE_OBJECT deviceObject,
  _Inout_ PIRP irp)
{
  if (BrokerDeviceIsControlDevice(deviceObject))
    return BrokerDeviceDispatch(deviceObject, irp);

  PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
  const bool removeDevice =
    stack->MajorFunction == IRP_MJ_PNP &&
    stack->MinorFunction == IRP_MN_REMOVE_DEVICE;
  if (stack->MajorFunction == IRP_MJ_PNP &&
      (removeDevice ||
       stack->MinorFunction == IRP_MN_STOP_DEVICE ||
       stack->MinorFunction == IRP_MN_SURPRISE_REMOVAL))
    BrokerDeviceDetachAdapter(deviceObject);

  const NTSTATUS status = PcDispatchIrp(deviceObject, irp);
  if (removeDevice)
    BrokerDeviceNotifyPnpDeviceRemoved();
  return status;
}

void DriverUnload(_In_ PDRIVER_OBJECT driverObject)
{
  BrokerDeviceShutdown();
  if (g_portClsUnload)
    g_portClsUnload(driverObject);
}

void InstallDispatchHooks(_In_ PDRIVER_OBJECT driverObject)
{
  constexpr UCHAR majors[] =
  {
    IRP_MJ_CLOSE,
    IRP_MJ_CREATE,
    IRP_MJ_DEVICE_CONTROL,
    IRP_MJ_FLUSH_BUFFERS,
    IRP_MJ_PNP,
    IRP_MJ_POWER,
    IRP_MJ_QUERY_SECURITY,
    IRP_MJ_READ,
    IRP_MJ_SET_SECURITY,
    IRP_MJ_SYSTEM_CONTROL,
    IRP_MJ_WRITE,
    IRP_MJ_CLEANUP
  };

  for (const UCHAR major : majors)
    driverObject->MajorFunction[major] = DispatchIrp;
}
}

extern "C" NTSTATUS DriverEntry(
  _In_ PDRIVER_OBJECT driverObject,
  _In_ PUNICODE_STRING registryPath)
{
  NTSTATUS status = PcInitializeAdapterDriver(driverObject, registryPath,
    AddAudioDevice);
  if (!NT_SUCCESS(status))
    return status;

  g_portClsUnload = driverObject->DriverUnload;
  InstallDispatchHooks(driverObject);
  driverObject->DriverUnload = DriverUnload;

  const NTSTATUS brokerStatus = BrokerDeviceInitialize(driverObject);
  if (!NT_SUCCESS(brokerStatus))
  {
    LGAUDIO_TRACE_ERROR("broker initialization unavailable: 0x%08X",
      brokerStatus);
  }
  return STATUS_SUCCESS;
}

#pragma warning(push)
#pragma warning(disable: 4996)
void * __cdecl operator new(
  size_t size,
  POOL_TYPE poolType,
  ULONG tag)
{
  void *memory = ExAllocatePoolWithTag(poolType, size, tag);
  if (memory)
    RtlZeroMemory(memory, size);
  return memory;
}

void * __cdecl operator new(
  size_t size,
  POOL_FLAGS poolFlags,
  ULONG tag)
{
  const POOL_TYPE poolType = (poolFlags & POOL_FLAG_PAGED) ?
    PagedPool : NonPagedPoolNx;
  return operator new(size, poolType, tag);
}
#pragma warning(pop)

void __cdecl operator delete(void *memory) noexcept
{
  if (memory)
    ExFreePool(memory);
}

void __cdecl operator delete(void *memory, size_t size) noexcept
{
  UNREFERENCED_PARAMETER(size);
  operator delete(memory);
}

void __cdecl operator delete(
  void *memory,
  POOL_TYPE poolType,
  ULONG tag) noexcept
{
  UNREFERENCED_PARAMETER(poolType);
  UNREFERENCED_PARAMETER(tag);
  operator delete(memory);
}
void __cdecl operator delete(
  void *memory,
  POOL_FLAGS poolFlags,
  ULONG tag) noexcept
{
  UNREFERENCED_PARAMETER(poolFlags);
  UNREFERENCED_PARAMETER(tag);
  operator delete(memory);
}
