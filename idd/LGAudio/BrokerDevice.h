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

#pragma once

#include "Common.h"
#include "Formats.h"

class CAudioAdapter;

struct LGAudioPortClsDeviceContext
{
  PVOID volatile adapter;
};

constexpr ULONG kLGAudioPortClsDeviceExtensionSize =
  PORT_CLASS_DEVICE_EXTENSION_SIZE +
  sizeof(LGAudioPortClsDeviceContext);

inline LGAudioPortClsDeviceContext *GetLGAudioDeviceContext(
  _In_ PDEVICE_OBJECT deviceObject)
{
  return reinterpret_cast<LGAudioPortClsDeviceContext *>(
    static_cast<BYTE *>(deviceObject->DeviceExtension) +
      PORT_CLASS_DEVICE_EXTENSION_SIZE);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS BrokerDeviceInitialize(_In_ PDRIVER_OBJECT driverObject);

_IRQL_requires_max_(PASSIVE_LEVEL)
void BrokerDeviceShutdown();

_IRQL_requires_max_(DISPATCH_LEVEL)
bool BrokerDeviceIsControlDevice(_In_ PDEVICE_OBJECT deviceObject);

_IRQL_requires_max_(APC_LEVEL)
NTSTATUS BrokerDeviceDispatch(
  _In_ PDEVICE_OBJECT deviceObject,
  _Inout_ PIRP irp);

_IRQL_requires_max_(PASSIVE_LEVEL)
void BrokerDeviceNotifyPnpDeviceAdded();

_IRQL_requires_max_(PASSIVE_LEVEL)
void BrokerDeviceNotifyPnpDeviceRemoved();

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS BrokerDeviceAttachAdapter(
  _In_ PDEVICE_OBJECT deviceObject,
  _In_ CAudioAdapter *adapter);

_IRQL_requires_max_(PASSIVE_LEVEL)
void BrokerDeviceDetachAdapter(_In_ PDEVICE_OBJECT deviceObject);

_IRQL_requires_max_(DISPATCH_LEVEL)
CAudioAdapter *BrokerDeviceAcquireAdapter(
  _Out_opt_ PULONGLONG generation);

_IRQL_requires_max_(DISPATCH_LEVEL)
void BrokerDeviceReleaseAdapter();

_IRQL_requires_max_(DISPATCH_LEVEL)
void BrokerDeviceNotifyStreamChanged(
  _In_ CAudioAdapter *adapter,
  _In_ EndpointKind kind);

_IRQL_requires_max_(DISPATCH_LEVEL)
void BrokerDeviceNotifyRenderAvailable(
  _In_ CAudioAdapter *adapter,
  _In_ ULONG bytes,
  _In_ ULONG droppedBytes);

_IRQL_requires_max_(DISPATCH_LEVEL)
void BrokerDeviceRecordCaptureUnderrun(
  _In_ CAudioAdapter *adapter,
  _In_ ULONGLONG bytes);
