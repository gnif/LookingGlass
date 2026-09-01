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
  constexpr ULONG kBrokerReadPoolTag = 'qBGL';
  constexpr LONG kMaximumPendingReads = 32;
  constexpr ULONG kReadServiceBatch = 8;

  struct BrokerFileContext
  {
    ULONG signature;
    bool bound;
    UCHAR reserved[3];
    ULONGLONG adapterGeneration;
    ULONGLONG bindingGeneration;
    ULONGLONG captureStreamGeneration;
    ULONGLONG nextCaptureSequence;
    CAudioAdapter *adapter;
  };

  struct BrokerControlExtension
  {
    KSPIN_LOCK stateLock;
    FAST_MUTEX bindingLock;
    FAST_MUTEX captureWriteLock;
    EX_RUNDOWN_REF bindingRundown;
    bool bindingRundownCompleted;
    PFILE_OBJECT owner;
    BrokerFileContext file;
    IO_CSQ readCsq;
    KSPIN_LOCK readQueueLock;
    LIST_ENTRY readQueue;
    KSPIN_LOCK pendingReadLock;
    LONG pendingReadCount;
    KEVENT readsDrainedEvent;
  };

  struct BrokerReadContext
  {
    ULONGLONG adapterGeneration;
    ULONGLONG bindingGeneration;
    ULONGLONG streamGeneration;
  };

  struct AdapterSnapshot
  {
    bool present;
    ULONGLONG generation;
    ULONGLONG eventSequence;
    AudioStreamSnapshot render;
    AudioStreamSnapshot capture;
  };

  struct BindingSnapshot
  {
    CAudioAdapter *adapter;
    ULONGLONG adapterGeneration;
    ULONGLONG bindingGeneration;
  };

  FAST_MUTEX g_controlLifecycleLock;
  FAST_MUTEX g_adapterLifecycleLock;
  KSPIN_LOCK g_adapterStateLock;
  EX_RUNDOWN_REF g_adapterRundown;
  EX_RUNDOWN_REF g_controlServiceRundown;
  KDPC g_readServiceDpc;

  PDRIVER_OBJECT g_driverObject;
  PDEVICE_OBJECT g_controlDevice;
  CAudioAdapter *g_adapter;
  PDEVICE_OBJECT g_adapterDevice;
  CAudioAdapter *g_boundAdapter;

  volatile LONG g_initialized;
  volatile LONG g_retiring;
  volatile LONG g_pnpDeviceCount;
  bool g_linkCreated;
  bool g_rundownCompleted;
  bool g_controlServiceRundownCompleted;

  volatile LONG64 g_adapterGeneration;
  volatile LONG64 g_eventSequence;
  volatile LONG64 g_bindingGeneration;
  volatile LONG64 g_bindCount;
  volatile LONG64 g_unbindCount;
  volatile LONG64 g_cleanupCount;
  volatile LONG64 g_rejectedRequestCount;
  volatile LONG64 g_renderPacketCount;
  volatile LONG64 g_renderByteCount;
  volatile LONG64 g_renderDropBytes;
  volatile LONG64 g_capturePacketCount;
  volatile LONG64 g_captureByteCount;
  volatile LONG64 g_captureDropBytes;
  volatile LONG64 g_captureUnderrunBytes;
  volatile LONG64 g_cancelledReadCount;
  volatile LONG64 g_renderSequence;

  void ReadServiceDpc(
    _In_ PKDPC dpc,
    _In_opt_ PVOID deferredContext,
    _In_opt_ PVOID systemArgument1,
    _In_opt_ PVOID systemArgument2);

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

  void AddCounter(
    _Inout_ volatile LONG64 *counter,
    _In_ ULONGLONG value)
  {
    InterlockedAdd64(counter, static_cast<LONG64>(value));
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

  CAudioAdapter *LoadBoundAdapter()
  {
    return static_cast<CAudioAdapter *>(
      InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile *>(&g_boundAdapter),
        nullptr, nullptr));
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

  LONG AddPendingRead(_In_ BrokerControlExtension *extension)
  {
    KIRQL oldIrql;
    KeAcquireSpinLock(&extension->pendingReadLock, &oldIrql);
    const LONG count = ++extension->pendingReadCount;
    if (count == 1)
      KeClearEvent(&extension->readsDrainedEvent);
    KeReleaseSpinLock(&extension->pendingReadLock, oldIrql);
    return count;
  }

  void ReleasePendingRead(_In_ BrokerControlExtension *extension)
  {
    KIRQL oldIrql;
    KeAcquireSpinLock(&extension->pendingReadLock, &oldIrql);
    NT_ASSERT(extension->pendingReadCount > 0);
    const LONG count = --extension->pendingReadCount;
    if (count == 0)
      KeSetEvent(&extension->readsDrainedEvent, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&extension->pendingReadLock, oldIrql);
  }

  bool HasPendingReads(_In_ BrokerControlExtension *extension)
  {
    KIRQL oldIrql;
    KeAcquireSpinLock(&extension->pendingReadLock, &oldIrql);
    const bool pending = extension->pendingReadCount != 0;
    KeReleaseSpinLock(&extension->pendingReadLock, oldIrql);
    return pending;
  }

  BrokerControlExtension *GetCsqExtension(_In_ PIO_CSQ csq)
  {
    return CONTAINING_RECORD(csq, BrokerControlExtension, readCsq);
  }

  void CsqInsertIrp(_In_ PIO_CSQ csq, _In_ PIRP irp)
  {
    BrokerControlExtension *extension = GetCsqExtension(csq);
    InsertTailList(&extension->readQueue,
      &irp->Tail.Overlay.ListEntry);
  }

  void CsqRemoveIrp(_In_ PIO_CSQ csq, _In_ PIRP irp)
  {
    UNREFERENCED_PARAMETER(csq);
    RemoveEntryList(&irp->Tail.Overlay.ListEntry);
  }

  PIRP CsqPeekNextIrp(
    _In_ PIO_CSQ csq,
    _In_opt_ PIRP irp,
    _In_opt_ PVOID peekContext)
  {
    BrokerControlExtension *extension = GetCsqExtension(csq);
    PLIST_ENTRY entry = irp ?
      irp->Tail.Overlay.ListEntry.Flink :
      extension->readQueue.Flink;

    while (entry != &extension->readQueue)
    {
      PIRP candidate = CONTAINING_RECORD(entry, IRP,
        Tail.Overlay.ListEntry);
      if (!peekContext ||
          IoGetCurrentIrpStackLocation(candidate)->FileObject ==
            peekContext)
      {
        return candidate;
      }
      entry = entry->Flink;
    }
    return nullptr;
  }

  void CsqAcquireLock(_In_ PIO_CSQ csq, _Out_ PKIRQL irql)
  {
    BrokerControlExtension *extension = GetCsqExtension(csq);
    KeAcquireSpinLock(&extension->readQueueLock, irql);
  }

  void CsqReleaseLock(_In_ PIO_CSQ csq, _In_ KIRQL irql)
  {
    BrokerControlExtension *extension = GetCsqExtension(csq);
    KeReleaseSpinLock(&extension->readQueueLock, irql);
  }

  void CsqCompleteCanceledIrp(_In_ PIO_CSQ csq, _In_ PIRP irp)
  {
    BrokerControlExtension *extension = GetCsqExtension(csq);
    auto *context = static_cast<BrokerReadContext *>(
      irp->Tail.Overlay.DriverContext[0]);
    irp->Tail.Overlay.DriverContext[0] = nullptr;
    if (context)
      ExFreePoolWithTag(context, kBrokerReadPoolTag);
    InterlockedIncrement64(&g_cancelledReadCount);
    CompleteIrp(irp, STATUS_CANCELLED);
    ReleasePendingRead(extension);
  }

  void SetReadContext(
    _Inout_ PIRP irp,
    _In_ BrokerReadContext *context)
  {
    irp->Tail.Overlay.DriverContext[0] = context;
  }

  BrokerReadContext *GetReadContext(_In_ PIRP irp)
  {
    return static_cast<BrokerReadContext *>(
      irp->Tail.Overlay.DriverContext[0]);
  }

  void FreeReadContext(_Inout_ PIRP irp)
  {
    BrokerReadContext *context = GetReadContext(irp);
    irp->Tail.Overlay.DriverContext[0] = nullptr;
    if (context)
      ExFreePoolWithTag(context, kBrokerReadPoolTag);
  }

  void CancelQueuedReads(
    _In_ BrokerControlExtension *extension,
    _In_opt_ PFILE_OBJECT fileObject,
    _In_ NTSTATUS status)
  {
    PIRP irp;
    while ((irp = IoCsqRemoveNextIrp(&extension->readCsq,
        fileObject)) != nullptr)
    {
      FreeReadContext(irp);
      InterlockedIncrement64(&g_cancelledReadCount);
      CompleteIrp(irp, status);
      ReleasePendingRead(extension);
    }
  }

  void DrainQueuedReads(
    _In_ BrokerControlExtension *extension,
    _In_opt_ PFILE_OBJECT fileObject,
    _In_ NTSTATUS status)
  {
    CancelQueuedReads(extension, fileObject, status);
    while (HasPendingReads(extension))
    {
      KeWaitForSingleObject(&extension->readsDrainedEvent,
        Executive, KernelMode, FALSE, nullptr);
    }
  }

  void QueueReadService()
  {
    if (!ExAcquireRundownProtection(&g_controlServiceRundown))
      return;

    if (LoadControlDevice() &&
        InterlockedCompareExchange(&g_retiring, 0, 0) == 0)
    {
      KeInsertQueueDpc(&g_readServiceDpc, nullptr, nullptr);
    }
    ExReleaseRundownProtection(&g_controlServiceRundown);
  }

  AdapterSnapshot GetAdapterSnapshot()
  {
    AdapterSnapshot snapshot{};

    if (!ExAcquireRundownProtection(&g_adapterRundown))
    {
      snapshot.generation = ReadCounter(&g_adapterGeneration);
      snapshot.eventSequence = ReadCounter(&g_eventSequence);
      return snapshot;
    }

    CAudioAdapter *adapter;
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_adapterStateLock, &oldIrql);
    adapter = g_adapter;
    snapshot.present = adapter != nullptr;
    snapshot.generation = ReadCounter(&g_adapterGeneration);
    snapshot.eventSequence = ReadCounter(&g_eventSequence);
    KeReleaseSpinLock(&g_adapterStateLock, oldIrql);

    if (adapter)
    {
      adapter->GetStreamSnapshot(EndpointKind::Render,
        &snapshot.render);
      adapter->GetStreamSnapshot(EndpointKind::Capture,
        &snapshot.capture);
    }
    ExReleaseRundownProtection(&g_adapterRundown);
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

  NTSTATUS AcquireBinding(
    _In_ BrokerControlExtension *extension,
    _In_opt_ PFILE_OBJECT fileObject,
    _Out_ BindingSnapshot *snapshot)
  {
    RtlZeroMemory(snapshot, sizeof(*snapshot));
    if (!ExAcquireRundownProtection(&extension->bindingRundown))
      return STATUS_DEVICE_NOT_READY;

    NTSTATUS status = STATUS_SUCCESS;
    KIRQL oldIrql;
    KeAcquireSpinLock(&extension->stateLock, &oldIrql);
    if (!IsOwnedFile(extension, fileObject))
      status = STATUS_INVALID_HANDLE;
    else if (!extension->file.bound || !extension->file.adapter)
      status = STATUS_DEVICE_NOT_READY;
    else
    {
      snapshot->adapter = extension->file.adapter;
      snapshot->adapterGeneration =
        extension->file.adapterGeneration;
      snapshot->bindingGeneration =
        extension->file.bindingGeneration;
    }
    KeReleaseSpinLock(&extension->stateLock, oldIrql);

    if (!NT_SUCCESS(status))
      ExReleaseRundownProtection(&extension->bindingRundown);
    return status;
  }

  bool IsCurrentAdapter(_In_ const BindingSnapshot &binding)
  {
    bool current;
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_adapterStateLock, &oldIrql);
    current = g_adapter == binding.adapter &&
      ReadCounter(&g_adapterGeneration) ==
        binding.adapterGeneration;
    KeReleaseSpinLock(&g_adapterStateLock, oldIrql);
    return current;
  }

  CAudioAdapter *UnbindFileLocked(
    _In_ BrokerControlExtension *extension)
  {
    if (!extension->file.bound)
      return nullptr;

    CAudioAdapter *adapter = extension->file.adapter;
    extension->file.bound = false;
    extension->file.adapterGeneration = 0;
    extension->file.bindingGeneration = 0;
    extension->file.captureStreamGeneration = 0;
    extension->file.nextCaptureSequence = 0;
    extension->file.adapter = nullptr;
    InterlockedCompareExchangePointer(
      reinterpret_cast<PVOID volatile *>(&g_boundAdapter),
      nullptr, adapter);
    InterlockedIncrement64(&g_bindingGeneration);
    return adapter;
  }

  void FinishUnbind(
    _In_ BrokerControlExtension *extension,
    _In_ CAudioAdapter *adapter)
  {
    if (!adapter)
      return;

    adapter->SetBrokerActive(false);
    ExWaitForRundownProtectionRelease(&extension->bindingRundown);
    extension->bindingRundownCompleted = true;
    adapter->Release();

    InterlockedIncrement64(&g_unbindCount);
    IncrementEventSequence();
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
    CAudioAdapter *adapter;

    ExAcquireFastMutex(&extension->bindingLock);
    KIRQL oldIrql;
    KeAcquireSpinLock(&extension->stateLock, &oldIrql);
    adapter = UnbindFileLocked(extension);
    KeReleaseSpinLock(&extension->stateLock, oldIrql);
    FinishUnbind(extension, adapter);
    DrainQueuedReads(extension, nullptr, STATUS_DEVICE_NOT_CONNECTED);
    ExReleaseFastMutex(&extension->bindingLock);
    ExReleaseFastMutex(&g_controlLifecycleLock);
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

  void StartControlServiceLocked()
  {
    if (!g_controlServiceRundownCompleted)
      return;

    KeRemoveQueueDpc(&g_readServiceDpc);
    ExReInitializeRundownProtection(&g_controlServiceRundown);
    g_controlServiceRundownCompleted = false;
  }

  void StopControlServiceLocked()
  {
    if (g_controlServiceRundownCompleted)
      return;

    KeRemoveQueueDpc(&g_readServiceDpc);
    ExWaitForRundownProtectionRelease(&g_controlServiceRundown);
    KeRemoveQueueDpc(&g_readServiceDpc);
    g_controlServiceRundownCompleted = true;
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
    ExInitializeFastMutex(&extension->bindingLock);
    ExInitializeFastMutex(&extension->captureWriteLock);
    ExInitializeRundownProtection(&extension->bindingRundown);
    KeInitializeSpinLock(&extension->readQueueLock);
    InitializeListHead(&extension->readQueue);
    KeInitializeSpinLock(&extension->pendingReadLock);
    KeInitializeEvent(&extension->readsDrainedEvent,
      NotificationEvent, TRUE);

    status = IoCsqInitialize(&extension->readCsq,
      CsqInsertIrp, CsqRemoveIrp, CsqPeekNextIrp,
      CsqAcquireLock, CsqReleaseLock, CsqCompleteCanceledIrp);
    if (!NT_SUCCESS(status))
    {
      IoDeleteDevice(deviceObject);
      return status;
    }

    deviceObject->Flags |= DO_DIRECT_IO;

    StartControlServiceLocked();
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
        !extension->owner && !HasPendingReads(extension))
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
      extension->file.captureStreamGeneration = 0;
      extension->file.nextCaptureSequence = 0;
      extension->file.adapter = nullptr;
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
    CAudioAdapter *adapter = nullptr;
    bool owned;

    ExAcquireFastMutex(&extension->bindingLock);
    KIRQL oldIrql;
    KeAcquireSpinLock(&extension->stateLock, &oldIrql);
    owned = IsOwnedFile(extension, stack->FileObject);
    if (owned)
      adapter = UnbindFileLocked(extension);
    KeReleaseSpinLock(&extension->stateLock, oldIrql);
    FinishUnbind(extension, adapter);
    if (owned)
      DrainQueuedReads(extension, stack->FileObject, STATUS_CANCELLED);
    ExReleaseFastMutex(&extension->bindingLock);

    InterlockedIncrement64(&g_cleanupCount);
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
    CAudioAdapter *adapter = nullptr;
    bool owned;

    ExAcquireFastMutex(&extension->bindingLock);
    KIRQL oldIrql;
    KeAcquireSpinLock(&extension->stateLock, &oldIrql);
    owned = IsOwnedFile(extension, fileObject);
    if (owned)
      adapter = UnbindFileLocked(extension);
    KeReleaseSpinLock(&extension->stateLock, oldIrql);

    FinishUnbind(extension, adapter);
    if (owned)
      DrainQueuedReads(extension, fileObject, STATUS_CANCELLED);

    KeAcquireSpinLock(&extension->stateLock, &oldIrql);
    if (IsOwnedFile(extension, fileObject))
    {
      extension->owner = nullptr;
      extension->file.signature = 0;
      if (fileObject)
        fileObject->FsContext = nullptr;
    }
    KeReleaseSpinLock(&extension->stateLock, oldIrql);
    ExReleaseFastMutex(&extension->bindingLock);

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

  bool ValidatePacketFormat(
    _In_ const LGAudioBrokerFormat &format,
    _Out_opt_ FormatFingerprint *fingerprint)
  {
    if (!format.sampleRate ||
        format.sampleRate > kMaximumSampleRate ||
        !format.channels || format.channels > 8 ||
        (format.containerBits != 16 && format.containerBits != 24) ||
        format.validBits != format.containerBits ||
        format.blockAlign !=
          format.channels * (format.containerBits / 8))
    {
      return false;
    }

    if (fingerprint)
    {
      fingerprint->sampleRate = format.sampleRate;
      fingerprint->channelMask = format.channelMask;
      fingerprint->channels = format.channels;
      fingerprint->containerBits = format.containerBits;
      fingerprint->validBits = format.validBits;
      fingerprint->blockAlign = format.blockAlign;
    }
    return true;
  }

  void CopyBrokerFormat(
    _In_ const FormatFingerprint &source,
    _Out_ LGAudioBrokerFormat *destination)
  {
    destination->sampleRate = source.sampleRate;
    destination->channelMask = source.channelMask;
    destination->channels = source.channels;
    destination->containerBits = source.containerBits;
    destination->validBits = source.validBits;
    destination->blockAlign = source.blockAlign;
  }

  UINT32 GetBrokerStreamState(_In_ KSSTATE state)
  {
    switch (state)
    {
      case KSSTATE_STOP:
        return LGAUDIO_BROKER_STREAM_STOPPED;
      case KSSTATE_ACQUIRE:
        return LGAUDIO_BROKER_STREAM_ACQUIRED;
      case KSSTATE_PAUSE:
        return LGAUDIO_BROKER_STREAM_PAUSED;
      case KSSTATE_RUN:
        return LGAUDIO_BROKER_STREAM_RUNNING;
      default:
        return LGAUDIO_BROKER_STREAM_UNAVAILABLE;
    }
  }

  void FillBrokerStreamState(
    _In_ const AudioStreamSnapshot &source,
    _Out_ LGAudioBrokerStreamState *destination)
  {
    destination->generation = source.generation;
    destination->state = GetBrokerStreamState(source.state);
    if (source.formatValid)
    {
      destination->flags |= LGAUDIO_BROKER_STREAM_FORMAT_VALID;
      CopyBrokerFormat(source.format, &destination->format);
    }
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
    caps->capabilities = LGAUDIO_BROKER_CAP_CONTROL |
      LGAUDIO_BROKER_CAP_RENDER_READ |
      LGAUDIO_BROKER_CAP_CAPTURE_WRITE;
    caps->maximumRenderTransfer = LGAUDIO_BROKER_MAX_PAYLOAD_BYTES;
    caps->maximumCaptureTransfer = LGAUDIO_BROKER_MAX_PAYLOAD_BYTES;
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
        extension->file.adapterGeneration == snapshot.generation &&
        extension->file.adapter == LoadBoundAdapter();
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
    if (snapshot.present)
    {
      state->flags |= LGAUDIO_BROKER_STATE_ADAPTER_PRESENT;
      FillBrokerStreamState(snapshot.render, &state->render);
      FillBrokerStreamState(snapshot.capture, &state->capture);
    }
    else
    {
      state->render.state = LGAUDIO_BROKER_STREAM_UNAVAILABLE;
      state->capture.state = LGAUDIO_BROKER_STREAM_UNAVAILABLE;
    }
    if (bound)
    {
      state->flags |= LGAUDIO_BROKER_STATE_BOUND |
        LGAUDIO_BROKER_STATE_DATA_PLANE_ACTIVE;
    }

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
      ExAcquireFastMutex(&extension->bindingLock);
      if (extension->bindingRundownCompleted)
      {
        ExReInitializeRundownProtection(&extension->bindingRundown);
        extension->bindingRundownCompleted = false;
      }

      KIRQL oldIrql;
      KeAcquireSpinLock(&extension->stateLock, &oldIrql);
      if (!IsOwnedFile(extension, fileObject))
        status = STATUS_INVALID_HANDLE;
      else if (extension->file.bound)
        status = STATUS_INVALID_DEVICE_STATE;
      else if (LoadBoundAdapter())
        status = STATUS_DEVICE_BUSY;
      KeReleaseSpinLock(&extension->stateLock, oldIrql);

      if (NT_SUCCESS(status))
      {
        const ULONGLONG bindingGeneration = static_cast<ULONGLONG>(
          InterlockedIncrement64(&g_bindingGeneration));
        adapter->AddRef();
        adapter->SetBrokerActive(true);
        InterlockedExchangePointer(
          reinterpret_cast<PVOID volatile *>(&g_boundAdapter), adapter);

        KeAcquireSpinLock(&extension->stateLock, &oldIrql);
        extension->file.bound = true;
        extension->file.adapterGeneration = generation;
        extension->file.bindingGeneration = bindingGeneration;
        extension->file.captureStreamGeneration = 0;
        extension->file.nextCaptureSequence = 0;
        extension->file.adapter = adapter;
        bind->bindingGeneration = bindingGeneration;
        KeReleaseSpinLock(&extension->stateLock, oldIrql);
      }
      ExReleaseFastMutex(&extension->bindingLock);
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

    CAudioAdapter *adapter = nullptr;
    NTSTATUS status = STATUS_SUCCESS;
    ExAcquireFastMutex(&extension->bindingLock);
    KIRQL oldIrql;
    KeAcquireSpinLock(&extension->stateLock, &oldIrql);
    if (!IsOwnedFile(extension, fileObject))
      status = STATUS_INVALID_HANDLE;
    else
      adapter = UnbindFileLocked(extension);
    KeReleaseSpinLock(&extension->stateLock, oldIrql);

    FinishUnbind(extension, adapter);
    DrainQueuedReads(extension, fileObject, STATUS_CANCELLED);
    ExReleaseFastMutex(&extension->bindingLock);
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
    stats->renderPacketCount = ReadCounter(&g_renderPacketCount);
    stats->renderByteCount = ReadCounter(&g_renderByteCount);
    stats->renderDropBytes = ReadCounter(&g_renderDropBytes);
    stats->capturePacketCount = ReadCounter(&g_capturePacketCount);
    stats->captureByteCount = ReadCounter(&g_captureByteCount);
    stats->captureDropBytes = ReadCounter(&g_captureDropBytes);
    stats->captureUnderrunBytes =
      ReadCounter(&g_captureUnderrunBytes);
    stats->cancelledReadCount = ReadCounter(&g_cancelledReadCount);
    return CompleteIrp(irp, STATUS_SUCCESS, sizeof(*stats));
  }

  void *MapDirectBuffer(
    _In_ PIRP irp,
    _In_ ULONG requiredBytes)
  {
    if (!irp->MdlAddress ||
        MmGetMdlByteCount(irp->MdlAddress) < requiredBytes)
    {
      return nullptr;
    }

    return MmGetSystemAddressForMdlSafe(irp->MdlAddress,
      NormalPagePriority | MdlMappingNoExecute);
  }

  void CompletePendingRead(
    _In_ BrokerControlExtension *extension,
    _Inout_ PIRP irp,
    _In_ NTSTATUS status,
    _In_ ULONG_PTR information = 0)
  {
    FreeReadContext(irp);
    if (status == STATUS_CANCELLED ||
        status == STATUS_DEVICE_NOT_READY ||
        status == STATUS_DEVICE_NOT_CONNECTED ||
        status == STATUS_RETRY)
    {
      InterlockedIncrement64(&g_cancelledReadCount);
    }
    CompleteIrp(irp, status, information);
    ReleasePendingRead(extension);
  }

  NTSTATUS DispatchRead(
    _In_ PDEVICE_OBJECT deviceObject,
    _Inout_ PIRP irp)
  {
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    const ULONG length = stack->Parameters.Read.Length;
    if (length <= sizeof(LGAudioBrokerPacketHeader) ||
        length > sizeof(LGAudioBrokerPacketHeader) +
          LGAUDIO_BROKER_MAX_PAYLOAD_BYTES ||
        !MapDirectBuffer(irp, length))
    {
      return CompleteIrp(irp, STATUS_INVALID_PARAMETER);
    }

    BrokerControlExtension *extension =
      GetControlExtension(deviceObject);
    BindingSnapshot binding{};
    NTSTATUS status = AcquireBinding(extension, stack->FileObject,
      &binding);
    if (!NT_SUCCESS(status))
      return CompleteIrp(irp, status);

    AudioStreamSnapshot render{};
    binding.adapter->GetStreamSnapshot(EndpointKind::Render, &render);
    const ULONG payloadCapacity =
      length - sizeof(LGAudioBrokerPacketHeader);
    if (!IsCurrentAdapter(binding))
      status = STATUS_DEVICE_NOT_CONNECTED;
    else if (!render.formatValid || render.state != KSSTATE_RUN)
      status = STATUS_DEVICE_NOT_READY;
    else if (!render.format.blockAlign ||
             payloadCapacity < render.format.blockAlign)
      status = STATUS_INVALID_PARAMETER;

    if (!NT_SUCCESS(status))
    {
      ExReleaseRundownProtection(&extension->bindingRundown);
      return CompleteIrp(irp, status);
    }

#pragma warning(push)
#pragma warning(disable:4996) // Required for pre-2004 Windows 10 releases.
    auto *context = static_cast<BrokerReadContext *>(
      ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(BrokerReadContext),
        kBrokerReadPoolTag));
#pragma warning(pop)
    if (!context)
    {
      ExReleaseRundownProtection(&extension->bindingRundown);
      return CompleteIrp(irp, STATUS_INSUFFICIENT_RESOURCES);
    }

    context->adapterGeneration = binding.adapterGeneration;
    context->bindingGeneration = binding.bindingGeneration;
    context->streamGeneration = render.generation;
    SetReadContext(irp, context);

    const LONG pending = AddPendingRead(extension);
    if (pending > kMaximumPendingReads)
    {
      ReleasePendingRead(extension);
      FreeReadContext(irp);
      ExReleaseRundownProtection(&extension->bindingRundown);
      return CompleteIrp(irp, STATUS_DEVICE_BUSY);
    }

    IoMarkIrpPending(irp);
    IoCsqInsertIrp(&extension->readCsq, irp, nullptr);
    ExReleaseRundownProtection(&extension->bindingRundown);
    QueueReadService();
    return STATUS_PENDING;
  }

  bool ServiceReadIrp(
    _In_ BrokerControlExtension *extension,
    _Inout_ PIRP irp)
  {
    BrokerReadContext *context = GetReadContext(irp);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    if (!context ||
        !ExAcquireRundownProtection(&extension->bindingRundown))
    {
      CompletePendingRead(extension, irp, STATUS_DEVICE_NOT_READY);
      return true;
    }

    BindingSnapshot binding{};
    NTSTATUS status = STATUS_SUCCESS;
    KIRQL oldIrql;
    KeAcquireSpinLock(&extension->stateLock, &oldIrql);
    if (!IsOwnedFile(extension, stack->FileObject) ||
        !extension->file.bound || !extension->file.adapter)
    {
      status = STATUS_DEVICE_NOT_READY;
    }
    else
    {
      binding.adapter = extension->file.adapter;
      binding.adapterGeneration = extension->file.adapterGeneration;
      binding.bindingGeneration = extension->file.bindingGeneration;
    }
    KeReleaseSpinLock(&extension->stateLock, oldIrql);

    if (NT_SUCCESS(status) &&
        (binding.adapterGeneration != context->adapterGeneration ||
         binding.bindingGeneration != context->bindingGeneration))
    {
      status = STATUS_RETRY;
    }
    if (NT_SUCCESS(status) && !IsCurrentAdapter(binding))
      status = STATUS_DEVICE_NOT_CONNECTED;

    AudioStreamSnapshot render{};
    if (NT_SUCCESS(status))
    {
      binding.adapter->GetStreamSnapshot(EndpointKind::Render, &render);
      if (!render.formatValid || render.state != KSSTATE_RUN)
        status = STATUS_DEVICE_NOT_READY;
      else if (render.generation != context->streamGeneration)
        status = STATUS_RETRY;
    }

    const ULONG length = stack->Parameters.Read.Length;
    auto *buffer = static_cast<BYTE *>(MapDirectBuffer(irp, length));
    if (NT_SUCCESS(status) && !buffer)
      status = STATUS_INSUFFICIENT_RESOURCES;

    ULONG bytesRead = 0;
    bool discontinuity = false;
    if (NT_SUCCESS(status))
    {
      const ULONG capacity =
        length - sizeof(LGAudioBrokerPacketHeader);
      BYTE *payload = buffer + sizeof(LGAudioBrokerPacketHeader);
      status = binding.adapter->ReadRenderFrames(render.format,
        render.generation, payload, capacity, &bytesRead,
        &discontinuity);
    }

    if (NT_SUCCESS(status) && !bytesRead)
    {
      IoCsqInsertIrp(&extension->readCsq, irp, nullptr);
      ExReleaseRundownProtection(&extension->bindingRundown);
      return false;
    }

    if (NT_SUCCESS(status))
    {
      LGAudioBrokerPacketHeader packet{};
      packet.size = sizeof(packet);
      packet.version = LGAUDIO_BROKER_ABI_VERSION;
      packet.flags = discontinuity ?
        LGAUDIO_BROKER_PACKET_DISCONTINUITY : 0;
      packet.direction = LGAUDIO_BROKER_PACKET_RENDER;
      packet.adapterGeneration = binding.adapterGeneration;
      packet.bindingGeneration = binding.bindingGeneration;
      packet.streamGeneration = render.generation;
      packet.sequence = static_cast<ULONGLONG>(
        InterlockedIncrement64(&g_renderSequence) - 1);
      CopyBrokerFormat(render.format, &packet.format);
      packet.frameCount = bytesRead / render.format.blockAlign;
      packet.payloadBytes = bytesRead;
      RtlCopyMemory(buffer, &packet, sizeof(packet));

      InterlockedIncrement64(&g_renderPacketCount);
      AddCounter(&g_renderByteCount, bytesRead);
    }

    const ULONG_PTR information = NT_SUCCESS(status) ?
      sizeof(LGAudioBrokerPacketHeader) + bytesRead : 0;
    CompletePendingRead(extension, irp, status, information);
    ExReleaseRundownProtection(&extension->bindingRundown);
    return true;
  }

  void ReadServiceDpc(
    _In_ PKDPC dpc,
    _In_opt_ PVOID deferredContext,
    _In_opt_ PVOID systemArgument1,
    _In_opt_ PVOID systemArgument2)
  {
    UNREFERENCED_PARAMETER(dpc);
    UNREFERENCED_PARAMETER(deferredContext);
    UNREFERENCED_PARAMETER(systemArgument1);
    UNREFERENCED_PARAMETER(systemArgument2);

    if (!ExAcquireRundownProtection(&g_controlServiceRundown))
      return;

    PDEVICE_OBJECT controlDevice = LoadControlDevice();
    ULONG serviced = 0;
    if (controlDevice)
    {
      BrokerControlExtension *extension =
        GetControlExtension(controlDevice);
      while (serviced < kReadServiceBatch)
      {
        PIRP irp = IoCsqRemoveNextIrp(&extension->readCsq, nullptr);
        if (!irp)
          break;
        if (!ServiceReadIrp(extension, irp))
          break;
        ++serviced;
      }

      if (serviced == kReadServiceBatch &&
          HasPendingReads(extension))
      {
        KeInsertQueueDpc(&g_readServiceDpc, nullptr, nullptr);
      }
    }

    ExReleaseRundownProtection(&g_controlServiceRundown);
  }

  NTSTATUS DispatchWrite(
    _In_ PDEVICE_OBJECT deviceObject,
    _Inout_ PIRP irp)
  {
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    const ULONG length = stack->Parameters.Write.Length;
    if (length <= sizeof(LGAudioBrokerPacketHeader) ||
        length > sizeof(LGAudioBrokerPacketHeader) +
          LGAUDIO_BROKER_MAX_PAYLOAD_BYTES)
    {
      return CompleteIrp(irp, STATUS_INVALID_PARAMETER);
    }

    const auto *mapped = static_cast<const BYTE *>(
      MapDirectBuffer(irp, length));
    if (!mapped)
      return CompleteIrp(irp, STATUS_INVALID_PARAMETER);

    LGAudioBrokerPacketHeader packet{};
    RtlCopyMemory(&packet, mapped, sizeof(packet));
    FormatFingerprint format{};
    const UINT32 allowedFlags =
      LGAUDIO_BROKER_PACKET_DISCONTINUITY;
    if (packet.size != sizeof(packet) ||
        packet.version != LGAUDIO_BROKER_ABI_VERSION ||
        (packet.flags & ~allowedFlags) != 0 ||
        packet.direction != LGAUDIO_BROKER_PACKET_CAPTURE ||
        packet.reserved[0] || packet.reserved[1] ||
        !packet.payloadBytes ||
        packet.payloadBytes > LGAUDIO_BROKER_MAX_PAYLOAD_BYTES ||
        packet.payloadBytes != length - sizeof(packet) ||
        !ValidatePacketFormat(packet.format, &format) ||
        !packet.frameCount ||
        (packet.payloadBytes % format.blockAlign) != 0 ||
        packet.frameCount != packet.payloadBytes / format.blockAlign)
    {
      return CompleteIrp(irp, STATUS_INVALID_PARAMETER);
    }

    BrokerControlExtension *extension =
      GetControlExtension(deviceObject);
    ExAcquireFastMutex(&extension->captureWriteLock);

    BindingSnapshot binding{};
    NTSTATUS status = AcquireBinding(extension, stack->FileObject,
      &binding);
    if (NT_SUCCESS(status) &&
        (packet.adapterGeneration != binding.adapterGeneration ||
         packet.bindingGeneration != binding.bindingGeneration))
    {
      status = STATUS_RETRY;
    }
    if (NT_SUCCESS(status) && !IsCurrentAdapter(binding))
      status = STATUS_DEVICE_NOT_CONNECTED;

    AudioStreamSnapshot capture{};
    if (NT_SUCCESS(status))
    {
      binding.adapter->GetStreamSnapshot(EndpointKind::Capture,
        &capture);
      if (!capture.formatValid || capture.state != KSSTATE_RUN)
        status = STATUS_DEVICE_NOT_READY;
      else if (packet.streamGeneration != capture.generation ||
          !EqualFormatFingerprint(format, capture.format))
        status = STATUS_RETRY;
    }

    if (NT_SUCCESS(status))
    {
      KIRQL oldIrql;
      KeAcquireSpinLock(&extension->stateLock, &oldIrql);
      if (!extension->file.bound ||
          extension->file.adapter != binding.adapter ||
          extension->file.bindingGeneration !=
            binding.bindingGeneration)
      {
        status = STATUS_DEVICE_NOT_READY;
      }
      else if (extension->file.captureStreamGeneration !=
          capture.generation)
      {
        if (packet.sequence != 0)
          status = STATUS_RETRY;
      }
      else if (packet.sequence !=
          extension->file.nextCaptureSequence)
      {
        status = STATUS_RETRY;
      }
      KeReleaseSpinLock(&extension->stateLock, oldIrql);
    }

    ULONG droppedBytes = 0;
    if (NT_SUCCESS(status))
    {
      const BYTE *payload =
        mapped + sizeof(LGAudioBrokerPacketHeader);
      status = binding.adapter->WriteCaptureFrames(format,
        capture.generation,
        (packet.flags & LGAUDIO_BROKER_PACKET_DISCONTINUITY) != 0,
        payload, packet.payloadBytes, &droppedBytes);
    }

    if (NT_SUCCESS(status))
    {
      KIRQL oldIrql;
      KeAcquireSpinLock(&extension->stateLock, &oldIrql);
      if (!extension->file.bound ||
          extension->file.bindingGeneration !=
            binding.bindingGeneration)
      {
        status = STATUS_DEVICE_NOT_READY;
      }
      else
      {
        extension->file.captureStreamGeneration = capture.generation;
        extension->file.nextCaptureSequence = packet.sequence + 1;
      }
      KeReleaseSpinLock(&extension->stateLock, oldIrql);
    }

    if (NT_SUCCESS(status))
    {
      InterlockedIncrement64(&g_capturePacketCount);
      AddCounter(&g_captureByteCount, packet.payloadBytes);
      AddCounter(&g_captureDropBytes, droppedBytes);
    }

    ExReleaseFastMutex(&extension->captureWriteLock);
    const NTSTATUS completedStatus = CompleteIrp(irp, status,
      NT_SUCCESS(status) ? length : 0);
    if (binding.adapter)
      ExReleaseRundownProtection(&extension->bindingRundown);
    return completedStatus;
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
  ExInitializeRundownProtection(&g_controlServiceRundown);
  KeInitializeDpc(&g_readServiceDpc, ReadServiceDpc, nullptr);
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
  StopControlServiceLocked();
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
    {
      const NTSTATUS status = DispatchRead(deviceObject, irp);
      if (!NT_SUCCESS(status))
        InterlockedIncrement64(&g_rejectedRequestCount);
      return status;
    }

    case IRP_MJ_WRITE:
    {
      const NTSTATUS status = DispatchWrite(deviceObject, irp);
      if (!NT_SUCCESS(status))
        InterlockedIncrement64(&g_rejectedRequestCount);
      return status;
    }

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
      StartControlServiceLocked();
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
  StopControlServiceLocked();
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

_IRQL_requires_max_(DISPATCH_LEVEL)
void BrokerDeviceNotifyStreamChanged(
  _In_ CAudioAdapter *adapter,
  _In_ EndpointKind kind)
{
  bool current;
  KIRQL oldIrql;
  KeAcquireSpinLock(&g_adapterStateLock, &oldIrql);
  current = adapter && g_adapter == adapter;
  KeReleaseSpinLock(&g_adapterStateLock, oldIrql);
  if (!current)
    return;

  IncrementEventSequence();
  if (kind == EndpointKind::Render)
    QueueReadService();
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void BrokerDeviceNotifyRenderAvailable(
  _In_ CAudioAdapter *adapter,
  _In_ ULONG bytes,
  _In_ ULONG droppedBytes)
{
  UNREFERENCED_PARAMETER(bytes);
  if (!adapter || LoadBoundAdapter() != adapter)
    return;

  AddCounter(&g_renderDropBytes, droppedBytes);
  QueueReadService();
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void BrokerDeviceRecordCaptureUnderrun(
  _In_ CAudioAdapter *adapter,
  _In_ ULONGLONG bytes)
{
  if (adapter && LoadBoundAdapter() == adapter)
    AddCounter(&g_captureUnderrunBytes, bytes);
}
