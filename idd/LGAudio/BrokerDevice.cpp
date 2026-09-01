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

#include "BrokerDevice.h"

#include "Adapter.h"
#include "LGAudioBroker.h"

extern "C"
{
#include <wdmsec.h>
}

namespace
{
  constexpr ULONG kBrokerFileSignature = 'fBGL';

  struct BrokerFileContext
  {
    ULONG signature;
    bool bound;
    UCHAR reserved[3];
    ULONGLONG adapterGeneration;
    ULONGLONG bindingGeneration;
  };

  struct BrokerControlExtension
  {
    KSPIN_LOCK stateLock;
    PFILE_OBJECT owner;
    BrokerFileContext file;
  };

  struct AdapterSnapshot
  {
    bool present;
    ULONGLONG generation;
    ULONGLONG eventSequence;
  };

  FAST_MUTEX g_controlLifecycleLock;
  FAST_MUTEX g_adapterLifecycleLock;
  KSPIN_LOCK g_adapterStateLock;
  EX_RUNDOWN_REF g_adapterRundown;

  PDRIVER_OBJECT g_driverObject;
  PDEVICE_OBJECT g_controlDevice;
  CAudioAdapter *g_adapter;
  PDEVICE_OBJECT g_adapterDevice;

  volatile LONG g_initialized;
  volatile LONG g_retiring;
  volatile LONG g_pnpDeviceCount;
  bool g_linkCreated;
  bool g_rundownCompleted;

  volatile LONG64 g_adapterGeneration;
  volatile LONG64 g_eventSequence;
  volatile LONG64 g_bindingGeneration;
  volatile LONG64 g_bindCount;
  volatile LONG64 g_unbindCount;
  volatile LONG64 g_cleanupCount;
  volatile LONG64 g_rejectedRequestCount;

  BrokerControlExtension *GetControlExtension(
    _In_ PDEVICE_OBJECT deviceObject)
  {
    return static_cast<BrokerControlExtension *>(
      deviceObject->DeviceExtension);
  }

  ULONGLONG ReadCounter(_In_ volatile LONG64 *counter)
  {
    return static_cast<ULONGLONG>(
      InterlockedCompareExchange64(counter, 0, 0));
  }

  void IncrementEventSequence()
  {
    InterlockedIncrement64(&g_eventSequence);
  }

  PDEVICE_OBJECT LoadControlDevice()
  {
    return static_cast<PDEVICE_OBJECT>(
      InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile *>(&g_controlDevice),
        nullptr, nullptr));
  }

  PDEVICE_OBJECT ExchangeControlDevice(
    _In_opt_ PDEVICE_OBJECT deviceObject)
  {
    return static_cast<PDEVICE_OBJECT>(InterlockedExchangePointer(
      reinterpret_cast<PVOID volatile *>(&g_controlDevice),
      deviceObject));
  }

  NTSTATUS CompleteIrp(
    _Inout_ PIRP irp,
    _In_ NTSTATUS status,
    _In_ ULONG_PTR information = 0)
  {
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
  }

  AdapterSnapshot GetAdapterSnapshot()
  {
    AdapterSnapshot snapshot{};

    KIRQL oldIrql;
    KeAcquireSpinLock(&g_adapterStateLock, &oldIrql);
    snapshot.present = g_adapter != nullptr;
    snapshot.generation = ReadCounter(&g_adapterGeneration);
    snapshot.eventSequence = ReadCounter(&g_eventSequence);
    KeReleaseSpinLock(&g_adapterStateLock, oldIrql);
    return snapshot;
  }

  bool IsOwnedFile(
    _In_ BrokerControlExtension *extension,
    _In_opt_ PFILE_OBJECT fileObject)
  {
    return fileObject && extension->owner == fileObject &&
      fileObject->FsContext == &extension->file &&
      extension->file.signature == kBrokerFileSignature;
  }

  bool UnbindFileLocked(_In_ BrokerControlExtension *extension)
  {
    if (!extension->file.bound)
      return false;

    extension->file.bound = false;
    extension->file.adapterGeneration = 0;
    extension->file.bindingGeneration = 0;
    InterlockedIncrement64(&g_bindingGeneration);
    return true;
  }

  _IRQL_requires_max_(PASSIVE_LEVEL)
  void InvalidateControlBinding()
  {
    ExAcquireFastMutex(&g_controlLifecycleLock);
    PDEVICE_OBJECT controlDevice = LoadControlDevice();
    if (!controlDevice)
    {
      ExReleaseFastMutex(&g_controlLifecycleLock);
      return;
    }

    BrokerControlExtension *extension =
      GetControlExtension(controlDevice);
    bool unbound;

    KIRQL oldIrql;
    KeAcquireSpinLock(&extension->stateLock, &oldIrql);
    unbound = UnbindFileLocked(extension);
    KeReleaseSpinLock(&extension->stateLock, oldIrql);
    ExReleaseFastMutex(&g_controlLifecycleLock);

    if (unbound)
    {
      InterlockedIncrement64(&g_unbindCount);
      IncrementEventSequence();
    }
  }

  void DeleteSymbolicLinkLocked()
  {
    if (!g_linkCreated)
      return;

    UNICODE_STRING dosName =
      RTL_CONSTANT_STRING(LGAUDIO_BROKER_DOS_DEVICE_NAME);
    IoDeleteSymbolicLink(&dosName);
    g_linkCreated = false;
  }

  NTSTATUS CreateControlDeviceLocked()
  {
    if (LoadControlDevice())
      return STATUS_SUCCESS;
    if (!g_driverObject)
      return STATUS_INVALID_DEVICE_STATE;

    UNICODE_STRING ntName =
      RTL_CONSTANT_STRING(LGAUDIO_BROKER_NT_DEVICE_NAME);
    UNICODE_STRING dosName =
      RTL_CONSTANT_STRING(LGAUDIO_BROKER_DOS_DEVICE_NAME);
    UNICODE_STRING security = RTL_CONSTANT_STRING(
      L"D:P(A;;GA;;;SY)(A;;GA;;;UD)");

    PDEVICE_OBJECT deviceObject = nullptr;
    NTSTATUS status = IoCreateDeviceSecure(
      g_driverObject,
      sizeof(BrokerControlExtension),
      &ntName,
      FILE_DEVICE_UNKNOWN,
      FILE_DEVICE_SECURE_OPEN,
      FALSE,
      &security,
      &LGAUDIO_BROKER_DEVICE_CLASS_GUID,
      &deviceObject);
    if (!NT_SUCCESS(status))
      return status;

    BrokerControlExtension *extension =
      GetControlExtension(deviceObject);
    RtlZeroMemory(extension, sizeof(*extension));
    KeInitializeSpinLock(&extension->stateLock);

    deviceObject->Flags |= DO_DIRECT_IO;

    InterlockedExchange(&g_retiring, 0);
    const PDEVICE_OBJECT previous = static_cast<PDEVICE_OBJECT>(
      InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile *>(&g_controlDevice),
        deviceObject, nullptr));
    if (previous)
    {
      IoDeleteDevice(deviceObject);
      return STATUS_DEVICE_BUSY;
    }

    status = IoCreateSymbolicLink(&dosName, &ntName);
    if (!NT_SUCCESS(status))
    {
      const PDEVICE_OBJECT removed = ExchangeControlDevice(nullptr);
      NT_ASSERT(removed == deviceObject);
      UNREFERENCED_PARAMETER(removed);
      IoDeleteDevice(deviceObject);
      return status;
    }

    g_linkCreated = true;
    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
  }

  void SetControlRetiringLocked(_In_ bool retiring)
  {
    PDEVICE_OBJECT controlDevice = LoadControlDevice();
    if (!controlDevice)
      return;

    BrokerControlExtension *extension =
      GetControlExtension(controlDevice);
    KIRQL oldIrql;
    KeAcquireSpinLock(&extension->stateLock, &oldIrql);
    InterlockedExchange(&g_retiring, retiring ? 1 : 0);
    KeReleaseSpinLock(&extension->stateLock, oldIrql);
  }

  PDEVICE_OBJECT TakeRetiredControlDeviceLocked()
  {
    PDEVICE_OBJECT controlDevice = LoadControlDevice();
    if (!controlDevice)
      return nullptr;

    BrokerControlExtension *extension =
      GetControlExtension(controlDevice);
    bool takeDevice = false;

    KIRQL oldIrql;
    KeAcquireSpinLock(&extension->stateLock, &oldIrql);
    if (InterlockedCompareExchange(&g_retiring, 0, 0) != 0 &&
        !extension->owner)
    {
      const PDEVICE_OBJECT removed = ExchangeControlDevice(nullptr);
      NT_ASSERT(removed == controlDevice);
      takeDevice = removed == controlDevice;
    }
    KeReleaseSpinLock(&extension->stateLock, oldIrql);
    return takeDevice ? controlDevice : nullptr;
  }

  NTSTATUS DispatchCreate(
    _In_ PDEVICE_OBJECT deviceObject,
    _Inout_ PIRP irp)
  {
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    PFILE_OBJECT fileObject = stack->FileObject;
    if (!fileObject || fileObject->RelatedFileObject ||
        fileObject->FileName.Length != 0)
    {
      InterlockedIncrement64(&g_rejectedRequestCount);
      return CompleteIrp(irp, STATUS_INVALID_PARAMETER);
    }

    BrokerControlExtension *extension =
      GetControlExtension(deviceObject);
    NTSTATUS status = STATUS_SUCCESS;

    KIRQL oldIrql;
    KeAcquireSpinLock(&extension->stateLock, &oldIrql);
    if (InterlockedCompareExchange(&g_retiring, 0, 0) != 0)
      status = STATUS_DELETE_PENDING;
    else if (extension->owner)
      status = STATUS_SHARING_VIOLATION;
    else
    {
      extension->owner = fileObject;
      extension->file.signature = kBrokerFileSignature;
      extension->file.bound = false;
      extension->file.adapterGeneration = 0;
      extension->file.bindingGeneration = 0;
      fileObject->FsContext = &extension->file;
    }
    KeReleaseSpinLock(&extension->stateLock, oldIrql);

    if (!NT_SUCCESS(status))
      InterlockedIncrement64(&g_rejectedRequestCount);
    return CompleteIrp(irp, status);
  }

  NTSTATUS DispatchCleanup(
    _In_ PDEVICE_OBJECT deviceObject,
    _Inout_ PIRP irp)
  {
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    BrokerControlExtension *extension =
      GetControlExtension(deviceObject);
    bool unbound = false;

    KIRQL oldIrql;
    KeAcquireSpinLock(&extension->stateLock, &oldIrql);
    if (IsOwnedFile(extension, stack->FileObject))
      unbound = UnbindFileLocked(extension);
    KeReleaseSpinLock(&extension->stateLock, oldIrql);

    InterlockedIncrement64(&g_cleanupCount);
    if (unbound)
    {
      InterlockedIncrement64(&g_unbindCount);
      IncrementEventSequence();
    }
    return CompleteIrp(irp, STATUS_SUCCESS);
  }

  NTSTATUS DispatchClose(
    _In_ PDEVICE_OBJECT deviceObject,
    _Inout_ PIRP irp)
  {
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    PFILE_OBJECT fileObject = stack->FileObject;
    BrokerControlExtension *extension =
      GetControlExtension(deviceObject);
    bool unbound = false;

    KIRQL oldIrql;
    KeAcquireSpinLock(&extension->stateLock, &oldIrql);
    if (IsOwnedFile(extension, fileObject))
    {
      unbound = UnbindFileLocked(extension);
      extension->owner = nullptr;
      extension->file.signature = 0;
      if (fileObject)
        fileObject->FsContext = nullptr;
    }
    KeReleaseSpinLock(&extension->stateLock, oldIrql);

    if (unbound)
    {
      InterlockedIncrement64(&g_unbindCount);
      IncrementEventSequence();
    }

    const NTSTATUS status = CompleteIrp(irp, STATUS_SUCCESS);

    PDEVICE_OBJECT deleteDevice = nullptr;
    ExAcquireFastMutex(&g_controlLifecycleLock);
    deleteDevice = TakeRetiredControlDeviceLocked();
    ExReleaseFastMutex(&g_controlLifecycleLock);
    if (deleteDevice)
      IoDeleteDevice(deleteDevice);

    return status;
  }

  bool ValidateVersionedInput(
    _In_reads_bytes_(inputLength) const void *input,
    _In_ ULONG inputLength,
    _In_ ULONG requiredSize)
  {
    if (!input || inputLength < sizeof(LGAudioBrokerHeader))
      return false;

    const auto *header =
      static_cast<const LGAudioBrokerHeader *>(input);
    return header->size >= requiredSize &&
      header->size <= inputLength &&
      header->version == LGAUDIO_BROKER_ABI_VERSION;
  }

  NTSTATUS QueryCaps(
    _Inout_ PIRP irp,
    _In_ ULONG outputLength)
  {
    if (outputLength < sizeof(LGAudioBrokerCaps))
      return CompleteIrp(irp, STATUS_BUFFER_TOO_SMALL,
        sizeof(LGAudioBrokerCaps));

    auto *caps = static_cast<LGAudioBrokerCaps *>(
      irp->AssociatedIrp.SystemBuffer);
    RtlZeroMemory(caps, sizeof(*caps));
    caps->size = sizeof(*caps);
    caps->version = LGAUDIO_BROKER_ABI_VERSION;
    caps->minimumVersion = LGAUDIO_BROKER_ABI_VERSION;
    caps->maximumVersion = LGAUDIO_BROKER_ABI_VERSION;
    caps->capabilities = LGAUDIO_BROKER_CAP_CONTROL;
    return CompleteIrp(irp, STATUS_SUCCESS, sizeof(*caps));
  }

  NTSTATUS QueryState(
    _In_ BrokerControlExtension *extension,
    _In_ PFILE_OBJECT fileObject,
    _Inout_ PIRP irp,
    _In_ ULONG outputLength)
  {
    if (outputLength < sizeof(LGAudioBrokerState))
      return CompleteIrp(irp, STATUS_BUFFER_TOO_SMALL,
        sizeof(LGAudioBrokerState));

    const AdapterSnapshot snapshot = GetAdapterSnapshot();
    bool bound = false;
    ULONGLONG bindingGeneration;

    KIRQL oldIrql;
    KeAcquireSpinLock(&extension->stateLock, &oldIrql);
    bindingGeneration = ReadCounter(&g_bindingGeneration);
    if (IsOwnedFile(extension, fileObject))
    {
      bound = extension->file.bound && snapshot.present &&
        extension->file.adapterGeneration == snapshot.generation;
    }
    KeReleaseSpinLock(&extension->stateLock, oldIrql);

    auto *state = static_cast<LGAudioBrokerState *>(
      irp->AssociatedIrp.SystemBuffer);
    RtlZeroMemory(state, sizeof(*state));
    state->size = sizeof(*state);
    state->version = LGAUDIO_BROKER_ABI_VERSION;
    state->adapterGeneration = snapshot.generation;
    state->eventSequence = snapshot.eventSequence;
    state->bindingGeneration = bindingGeneration;
    state->render.state = LGAUDIO_BROKER_STREAM_UNAVAILABLE;
    state->capture.state = LGAUDIO_BROKER_STREAM_UNAVAILABLE;
    if (snapshot.present)
      state->flags |= LGAUDIO_BROKER_STATE_ADAPTER_PRESENT;
    if (bound)
      state->flags |= LGAUDIO_BROKER_STATE_BOUND;

    return CompleteIrp(irp, STATUS_SUCCESS, sizeof(*state));
  }

  NTSTATUS Bind(
    _In_ BrokerControlExtension *extension,
    _In_ PFILE_OBJECT fileObject,
    _Inout_ PIRP irp,
    _In_ ULONG inputLength,
    _In_ ULONG outputLength)
  {
    const void *input = irp->AssociatedIrp.SystemBuffer;
    if (!ValidateVersionedInput(input, inputLength,
        sizeof(LGAudioBrokerBind)))
      return CompleteIrp(irp, STATUS_INVALID_PARAMETER);

    if (outputLength < sizeof(LGAudioBrokerBind))
      return CompleteIrp(irp, STATUS_BUFFER_TOO_SMALL,
        sizeof(LGAudioBrokerBind));

    auto *bind = static_cast<LGAudioBrokerBind *>(
      irp->AssociatedIrp.SystemBuffer);
    if (bind->flags || bind->reserved0 || bind->reserved[0] ||
        bind->reserved[1] || bind->bindingGeneration)
      return CompleteIrp(irp, STATUS_INVALID_PARAMETER);

    ULONGLONG generation = 0;
    CAudioAdapter *adapter = BrokerDeviceAcquireAdapter(&generation);
    if (!adapter)
      return CompleteIrp(irp, STATUS_DEVICE_NOT_READY);

    NTSTATUS status = STATUS_SUCCESS;
    if (bind->adapterGeneration != generation)
      status = STATUS_RETRY;
    else
    {
      KIRQL oldIrql;
      KeAcquireSpinLock(&extension->stateLock, &oldIrql);
      if (!IsOwnedFile(extension, fileObject))
        status = STATUS_INVALID_HANDLE;
      else if (extension->file.bound)
        status = STATUS_INVALID_DEVICE_STATE;
      else
      {
        const ULONGLONG bindingGeneration = static_cast<ULONGLONG>(
          InterlockedIncrement64(&g_bindingGeneration));
        extension->file.bound = true;
        extension->file.adapterGeneration = generation;
        extension->file.bindingGeneration = bindingGeneration;
        bind->bindingGeneration = bindingGeneration;
      }
      KeReleaseSpinLock(&extension->stateLock, oldIrql);
    }

    BrokerDeviceReleaseAdapter();
    if (NT_SUCCESS(status))
    {
      InterlockedIncrement64(&g_bindCount);
      IncrementEventSequence();
    }
    return CompleteIrp(irp, status,
      NT_SUCCESS(status) ? sizeof(*bind) : 0);
  }

  NTSTATUS Unbind(
    _In_ BrokerControlExtension *extension,
    _In_ PFILE_OBJECT fileObject,
    _Inout_ PIRP irp,
    _In_ ULONG inputLength)
  {
    const void *input = irp->AssociatedIrp.SystemBuffer;
    if (!ValidateVersionedInput(input, inputLength,
        sizeof(LGAudioBrokerHeader)))
      return CompleteIrp(irp, STATUS_INVALID_PARAMETER);

    bool unbound = false;
    NTSTATUS status = STATUS_SUCCESS;
    KIRQL oldIrql;
    KeAcquireSpinLock(&extension->stateLock, &oldIrql);
    if (!IsOwnedFile(extension, fileObject))
      status = STATUS_INVALID_HANDLE;
    else
      unbound = UnbindFileLocked(extension);
    KeReleaseSpinLock(&extension->stateLock, oldIrql);

    if (unbound)
    {
      InterlockedIncrement64(&g_unbindCount);
      IncrementEventSequence();
    }
    return CompleteIrp(irp, status);
  }

  NTSTATUS QueryStats(
    _Inout_ PIRP irp,
    _In_ ULONG outputLength)
  {
    if (outputLength < sizeof(LGAudioBrokerStats))
      return CompleteIrp(irp, STATUS_BUFFER_TOO_SMALL,
        sizeof(LGAudioBrokerStats));

    auto *stats = static_cast<LGAudioBrokerStats *>(
      irp->AssociatedIrp.SystemBuffer);
    RtlZeroMemory(stats, sizeof(*stats));
    stats->size = sizeof(*stats);
    stats->version = LGAUDIO_BROKER_ABI_VERSION;
    stats->adapterGeneration = ReadCounter(&g_adapterGeneration);
    stats->eventSequence = ReadCounter(&g_eventSequence);
    stats->bindingGeneration = ReadCounter(&g_bindingGeneration);
    stats->bindCount = ReadCounter(&g_bindCount);
    stats->unbindCount = ReadCounter(&g_unbindCount);
    stats->cleanupCount = ReadCounter(&g_cleanupCount);
    stats->rejectedRequestCount =
      ReadCounter(&g_rejectedRequestCount);
    return CompleteIrp(irp, STATUS_SUCCESS, sizeof(*stats));
  }

  NTSTATUS DispatchDeviceControl(
    _In_ PDEVICE_OBJECT deviceObject,
    _Inout_ PIRP irp)
  {
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    PFILE_OBJECT fileObject = stack->FileObject;
    BrokerControlExtension *extension =
      GetControlExtension(deviceObject);

    KIRQL oldIrql;
    KeAcquireSpinLock(&extension->stateLock, &oldIrql);
    const bool owner = IsOwnedFile(extension, fileObject);
    KeReleaseSpinLock(&extension->stateLock, oldIrql);
    if (!owner)
    {
      InterlockedIncrement64(&g_rejectedRequestCount);
      return CompleteIrp(irp, STATUS_INVALID_HANDLE);
    }

    const ULONG inputLength =
      stack->Parameters.DeviceIoControl.InputBufferLength;
    const ULONG outputLength =
      stack->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS status;

    switch (stack->Parameters.DeviceIoControl.IoControlCode)
    {
      case IOCTL_LGAUDIO_BROKER_QUERY_CAPS:
        status = QueryCaps(irp, outputLength);
        break;

      case IOCTL_LGAUDIO_BROKER_QUERY_STATE:
        status = QueryState(extension, fileObject, irp, outputLength);
        break;

      case IOCTL_LGAUDIO_BROKER_BIND:
        status = Bind(extension, fileObject, irp, inputLength,
          outputLength);
        break;

      case IOCTL_LGAUDIO_BROKER_UNBIND:
        status = Unbind(extension, fileObject, irp, inputLength);
        break;

      case IOCTL_LGAUDIO_BROKER_QUERY_STATS:
        status = QueryStats(irp, outputLength);
        break;

      default:
        status = CompleteIrp(irp, STATUS_INVALID_DEVICE_REQUEST);
        break;
    }

    if (!NT_SUCCESS(status))
      InterlockedIncrement64(&g_rejectedRequestCount);
    return status;
  }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS BrokerDeviceInitialize(_In_ PDRIVER_OBJECT driverObject)
{
  if (!driverObject)
    return STATUS_INVALID_PARAMETER;

  if (InterlockedCompareExchange(&g_initialized, 1, 0) != 0)
    return STATUS_SUCCESS;

  ExInitializeFastMutex(&g_controlLifecycleLock);
  ExInitializeFastMutex(&g_adapterLifecycleLock);
  KeInitializeSpinLock(&g_adapterStateLock);
  ExInitializeRundownProtection(&g_adapterRundown);
  g_driverObject = driverObject;

  ExAcquireFastMutex(&g_controlLifecycleLock);
  const NTSTATUS status = CreateControlDeviceLocked();
  ExReleaseFastMutex(&g_controlLifecycleLock);
  return status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void BrokerDeviceShutdown()
{
  if (InterlockedCompareExchange(&g_initialized, 0, 1) != 1)
    return;

  InvalidateControlBinding();

  ExAcquireFastMutex(&g_adapterLifecycleLock);
  bool waitForRundown = false;
  KIRQL oldIrql;
  KeAcquireSpinLock(&g_adapterStateLock, &oldIrql);
  if (g_adapter)
  {
    g_adapter = nullptr;
    g_adapterDevice = nullptr;
    InterlockedIncrement64(&g_adapterGeneration);
    IncrementEventSequence();
    waitForRundown = true;
  }
  KeReleaseSpinLock(&g_adapterStateLock, oldIrql);
  if (waitForRundown)
  {
    ExWaitForRundownProtectionRelease(&g_adapterRundown);
    g_rundownCompleted = true;
  }
  ExReleaseFastMutex(&g_adapterLifecycleLock);

  PDEVICE_OBJECT deleteDevice = nullptr;
  ExAcquireFastMutex(&g_controlLifecycleLock);
  SetControlRetiringLocked(true);
  DeleteSymbolicLinkLocked();
  deleteDevice = TakeRetiredControlDeviceLocked();
  NT_ASSERT(deleteDevice || !LoadControlDevice());
  ExReleaseFastMutex(&g_controlLifecycleLock);

  if (deleteDevice)
    IoDeleteDevice(deleteDevice);
  g_driverObject = nullptr;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
bool BrokerDeviceIsControlDevice(_In_ PDEVICE_OBJECT deviceObject)
{
  return deviceObject && deviceObject == LoadControlDevice();
}

_IRQL_requires_max_(APC_LEVEL)
NTSTATUS BrokerDeviceDispatch(
  _In_ PDEVICE_OBJECT deviceObject,
  _Inout_ PIRP irp)
{
  if (!deviceObject || !irp)
    return STATUS_INVALID_PARAMETER;

  const UCHAR major = IoGetCurrentIrpStackLocation(irp)->MajorFunction;
  switch (major)
  {
    case IRP_MJ_CREATE:
      return DispatchCreate(deviceObject, irp);

    case IRP_MJ_CLEANUP:
      return DispatchCleanup(deviceObject, irp);

    case IRP_MJ_CLOSE:
      return DispatchClose(deviceObject, irp);

    case IRP_MJ_DEVICE_CONTROL:
      return DispatchDeviceControl(deviceObject, irp);

    case IRP_MJ_FLUSH_BUFFERS:
      return CompleteIrp(irp, STATUS_SUCCESS);

    case IRP_MJ_READ:
    case IRP_MJ_WRITE:
      InterlockedIncrement64(&g_rejectedRequestCount);
      return CompleteIrp(irp, STATUS_NOT_SUPPORTED);

    default:
      InterlockedIncrement64(&g_rejectedRequestCount);
      return CompleteIrp(irp, STATUS_INVALID_DEVICE_REQUEST);
  }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void BrokerDeviceNotifyPnpDeviceAdded()
{
  const LONG count = InterlockedIncrement(&g_pnpDeviceCount);
  if (count != 1 || !g_initialized)
    return;

  ExAcquireFastMutex(&g_controlLifecycleLock);
  if (!LoadControlDevice())
  {
    const NTSTATUS status = CreateControlDeviceLocked();
    if (!NT_SUCCESS(status))
    {
      LGAUDIO_TRACE_ERROR("broker control device unavailable: 0x%08X",
        status);
    }
  }
  else if (InterlockedCompareExchange(&g_retiring, 0, 0) != 0)
  {
    UNICODE_STRING ntName =
      RTL_CONSTANT_STRING(LGAUDIO_BROKER_NT_DEVICE_NAME);
    UNICODE_STRING dosName =
      RTL_CONSTANT_STRING(LGAUDIO_BROKER_DOS_DEVICE_NAME);
    const NTSTATUS status = IoCreateSymbolicLink(&dosName, &ntName);
    if (NT_SUCCESS(status))
    {
      g_linkCreated = true;
      SetControlRetiringLocked(false);
    }
    else
    {
      LGAUDIO_TRACE_ERROR("broker link unavailable: 0x%08X", status);
    }
  }
  ExReleaseFastMutex(&g_controlLifecycleLock);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void BrokerDeviceNotifyPnpDeviceRemoved()
{
  LONG count = InterlockedDecrement(&g_pnpDeviceCount);
  if (count > 0)
    return;
  if (count < 0)
    InterlockedExchange(&g_pnpDeviceCount, 0);

  InvalidateControlBinding();

  PDEVICE_OBJECT deleteDevice = nullptr;
  ExAcquireFastMutex(&g_controlLifecycleLock);
  SetControlRetiringLocked(true);
  DeleteSymbolicLinkLocked();
  deleteDevice = TakeRetiredControlDeviceLocked();
  ExReleaseFastMutex(&g_controlLifecycleLock);

  if (deleteDevice)
    IoDeleteDevice(deleteDevice);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS BrokerDeviceAttachAdapter(
  _In_ PDEVICE_OBJECT deviceObject,
  _In_ CAudioAdapter *adapter)
{
  if (!deviceObject || !adapter || !g_initialized)
    return STATUS_INVALID_PARAMETER;

  LGAudioPortClsDeviceContext *deviceContext =
    GetLGAudioDeviceContext(deviceObject);
  adapter->AddRef();
  if (InterlockedCompareExchangePointer(&deviceContext->adapter,
      adapter, nullptr) != nullptr)
  {
    adapter->Release();
    return STATUS_DEVICE_BUSY;
  }

  NTSTATUS status = STATUS_SUCCESS;
  ExAcquireFastMutex(&g_adapterLifecycleLock);
  if (g_adapter)
    status = STATUS_DEVICE_BUSY;
  else
  {
    if (g_rundownCompleted)
    {
      ExReInitializeRundownProtection(&g_adapterRundown);
      g_rundownCompleted = false;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&g_adapterStateLock, &oldIrql);
    g_adapter = adapter;
    g_adapterDevice = deviceObject;
    InterlockedIncrement64(&g_adapterGeneration);
    IncrementEventSequence();
    KeReleaseSpinLock(&g_adapterStateLock, oldIrql);
  }
  ExReleaseFastMutex(&g_adapterLifecycleLock);

  if (!NT_SUCCESS(status))
  {
    InterlockedExchangePointer(&deviceContext->adapter, nullptr);
    adapter->Release();
  }
  return status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void BrokerDeviceDetachAdapter(_In_ PDEVICE_OBJECT deviceObject)
{
  if (!deviceObject || !g_initialized)
    return;

  LGAudioPortClsDeviceContext *deviceContext =
    GetLGAudioDeviceContext(deviceObject);
  auto *adapter = static_cast<CAudioAdapter *>(
    InterlockedExchangePointer(&deviceContext->adapter, nullptr));
  if (!adapter)
    return;

  bool detached = false;
  ExAcquireFastMutex(&g_adapterLifecycleLock);
  KIRQL oldIrql;
  KeAcquireSpinLock(&g_adapterStateLock, &oldIrql);
  if (g_adapter == adapter && g_adapterDevice == deviceObject)
  {
    g_adapter = nullptr;
    g_adapterDevice = nullptr;
    InterlockedIncrement64(&g_adapterGeneration);
    IncrementEventSequence();
    detached = true;
  }
  KeReleaseSpinLock(&g_adapterStateLock, oldIrql);

  if (detached)
  {
    ExWaitForRundownProtectionRelease(&g_adapterRundown);
    g_rundownCompleted = true;
    InvalidateControlBinding();
  }
  ExReleaseFastMutex(&g_adapterLifecycleLock);
  adapter->Release();
}

_IRQL_requires_max_(DISPATCH_LEVEL)
CAudioAdapter *BrokerDeviceAcquireAdapter(
  _Out_opt_ PULONGLONG generation)
{
  if (generation)
    *generation = 0;
  if (!g_initialized ||
      !ExAcquireRundownProtection(&g_adapterRundown))
    return nullptr;

  CAudioAdapter *adapter;
  KIRQL oldIrql;
  KeAcquireSpinLock(&g_adapterStateLock, &oldIrql);
  adapter = g_adapter;
  if (adapter && generation)
    *generation = ReadCounter(&g_adapterGeneration);
  KeReleaseSpinLock(&g_adapterStateLock, oldIrql);

  if (!adapter)
    ExReleaseRundownProtection(&g_adapterRundown);
  return adapter;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void BrokerDeviceReleaseAdapter()
{
  ExReleaseRundownProtection(&g_adapterRundown);
}
