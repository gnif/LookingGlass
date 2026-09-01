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

extern "C" DRIVER_INITIALIZE DriverEntry;

namespace
{
NTSTATUS AddAudioDevice(
  _In_ PDRIVER_OBJECT driverObject,
  _In_ PDEVICE_OBJECT physicalDeviceObject)
{
  constexpr ULONG kSubdeviceCount = 4;
  return PcAddAdapterDevice(driverObject, physicalDeviceObject,
    StartAudioDevice, kSubdeviceCount, 0);
}
}

extern "C" NTSTATUS DriverEntry(
  _In_ PDRIVER_OBJECT driverObject,
  _In_ PUNICODE_STRING registryPath)
{
  return PcInitializeAdapterDriver(driverObject, registryPath,
    AddAudioDevice);
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
