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

#include "CHIDDevice.h"

#include "CSRWLock.h"
#include "HIDReports.h"

#include <hidport.h>

static constexpr USHORT LG_INPUT_VENDOR_ID  = 0x0000;
static constexpr USHORT LG_INPUT_PRODUCT_ID = 0x0000;
static constexpr USHORT LG_INPUT_VERSION    = 0x0001;
static constexpr size_t REPORT_QUEUE_LENGTH = 64;

struct HIDQueuedReport
{
  UCHAR  data[sizeof(HIDKeyboardReport)];
  size_t size;
};

struct HIDDeviceContext
{
  WDFQUEUE              reportQueue;
  HID_DEVICE_ATTRIBUTES attributes;
  HID_DESCRIPTOR        descriptor;
  UCHAR                 keyboardLeds;
  SRWLOCK               reportLock;
  bool                  active;
  bool                  stopping;
  size_t                reportHead;
  size_t                reportCount;
  HIDQueuedReport       reports[REPORT_QUEUE_LENGTH];
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(HIDDeviceContext, HIDGetDeviceContext);

static SRWLOCK            s_deviceLock = SRWLOCK_INIT;
static HIDDeviceContext * s_device     = nullptr;

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL HIDEvtIoDeviceControl;
EVT_WDF_OBJECT_CONTEXT_CLEANUP HIDEvtReportQueueCleanup;

static NTSTATUS CopyToRequest(
  _In_ WDFREQUEST request,
  _In_reads_bytes_(size) const void * buffer,
  _In_ size_t size)
{
  WDFMEMORY memory;
  NTSTATUS  status = WdfRequestRetrieveOutputMemory(request, &memory);
  if (!NT_SUCCESS(status))
    return status;

  size_t bufferSize;
  WdfMemoryGetBuffer(memory, &bufferSize);
  if (bufferSize < size)
    return STATUS_BUFFER_TOO_SMALL;

  status = WdfMemoryCopyFromBuffer(
    memory, 0, const_cast<void *>(buffer), size);
  if (NT_SUCCESS(status))
    WdfRequestSetInformation(request, size);

  return status;
}

static NTSTATUS GetOutputReport(
  _In_ WDFREQUEST request,
  _Out_ HID_XFER_PACKET * packet)
{
  WDFMEMORY memory;
  NTSTATUS  status = WdfRequestRetrieveOutputMemory(request, &memory);
  if (!NT_SUCCESS(status))
    return status;

  // mshidumdf carries the report ID in the output buffer length.
  size_t outputBufferLength;
  WdfMemoryGetBuffer(memory, &outputBufferLength);
  packet->reportId = static_cast<UCHAR>(outputBufferLength);

  status = WdfRequestRetrieveInputMemory(request, &memory);
  if (!NT_SUCCESS(status))
    return status;

  size_t reportSize;
  packet->reportBuffer    =
    static_cast<PUCHAR>(WdfMemoryGetBuffer(memory, &reportSize));
  packet->reportBufferLen = static_cast<ULONG>(reportSize);
  return STATUS_SUCCESS;
}

static NTSTATUS SetOutputReport(
  _In_ WDFQUEUE queue,
  _In_ WDFREQUEST request)
{
  HID_XFER_PACKET packet = {};
  NTSTATUS        status = GetOutputReport(request, &packet);
  if (!NT_SUCCESS(status))
    return status;

  if (packet.reportId != HID_REPORT_ID_KEYBOARD ||
      packet.reportBufferLen < sizeof(HIDKeyboardLedsReport))
    return STATUS_INVALID_PARAMETER;

  const HIDKeyboardLedsReport * report =
    reinterpret_cast<const HIDKeyboardLedsReport *>(packet.reportBuffer);
  if (report->reportId != HID_REPORT_ID_KEYBOARD)
    return STATUS_INVALID_PARAMETER;

  WDFDEVICE device = WdfIoQueueGetDevice(queue);
  HIDGetDeviceContext(device)->keyboardLeds = report->leds;
  WdfRequestSetInformation(request, sizeof(*report));
  return STATUS_SUCCESS;
}

static void PopReport(
  _Inout_ HIDDeviceContext * context,
  _Out_ HIDQueuedReport * report)
{
  *report = context->reports[context->reportHead];
  context->reportHead =
    (context->reportHead + 1) % REPORT_QUEUE_LENGTH;
  --context->reportCount;
}

static NTSTATUS QueueReport(
  _Inout_ HIDDeviceContext * context,
  _In_reads_bytes_(size) const void * data,
  _In_ size_t size)
{
  if (context->reportCount == REPORT_QUEUE_LENGTH)
    return STATUS_BUFFER_OVERFLOW;

  const size_t index =
    (context->reportHead + context->reportCount) % REPORT_QUEUE_LENGTH;
  HIDQueuedReport * report = &context->reports[index];
  CopyMemory(report->data, data, size);
  report->size = size;
  ++context->reportCount;
  return STATUS_SUCCESS;
}

static NTSTATUS ReadReport(
  _In_ HIDDeviceContext * context,
  _In_ WDFREQUEST request,
  _Out_ bool * complete)
{
  HIDQueuedReport report = {};
  bool            haveReport = false;
  *complete = true;

  NTSTATUS status;
  {
    CSRWExclusiveLock lock(&context->reportLock);
    if (context->stopping || !context->active)
      status = STATUS_DEVICE_NOT_READY;
    else if (context->reportCount)
    {
      PopReport(context, &report);
      haveReport = true;
      status = STATUS_SUCCESS;
    }
    else
    {
      status = WdfRequestForwardToIoQueue(request, context->reportQueue);
      *complete = !NT_SUCCESS(status);
    }
  }

  if (haveReport)
    status = CopyToRequest(request, report.data, report.size);
  return status;
}

static NTSTATUS ActivateDevice(_Inout_ HIDDeviceContext * context)
{
  CSRWExclusiveLock lock(&context->reportLock);
  if (context->stopping)
    return STATUS_DEVICE_NOT_READY;

  WdfIoQueueStart(context->reportQueue);
  context->active = true;
  return STATUS_SUCCESS;
}

static NTSTATUS DeactivateDevice(_Inout_ HIDDeviceContext * context)
{
  CSRWExclusiveLock lock(&context->reportLock);
  if (context->stopping)
    return STATUS_DEVICE_NOT_READY;

  context->active      = false;
  context->reportHead  = 0;
  context->reportCount = 0;
  WdfIoQueuePurgeSynchronously(context->reportQueue);
  return STATUS_SUCCESS;
}

static NTSTATUS CreateQueues(_In_ WDFDEVICE device)
{
  WDF_IO_QUEUE_CONFIG queueConfig;
  WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
    &queueConfig, WdfIoQueueDispatchParallel);
  queueConfig.EvtIoDeviceControl = HIDEvtIoDeviceControl;

  NTSTATUS status = WdfIoQueueCreate(
    device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
  if (!NT_SUCCESS(status))
    return status;

  WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

  WDF_OBJECT_ATTRIBUTES queueAttributes;
  WDF_OBJECT_ATTRIBUTES_INIT(&queueAttributes);
  queueAttributes.EvtCleanupCallback = HIDEvtReportQueueCleanup;
  return WdfIoQueueCreate(
    device,
    &queueConfig,
    &queueAttributes,
    &HIDGetDeviceContext(device)->reportQueue);
}

NTSTATUS CHIDDevice::Create(_Inout_ PWDFDEVICE_INIT deviceInit)
{
  WdfFdoInitSetFilter(deviceInit);

  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, HIDDeviceContext);

  WDFDEVICE device;
  NTSTATUS  status = WdfDeviceCreate(&deviceInit, &attributes, &device);
  if (!NT_SUCCESS(status))
    return status;

  HIDDeviceContext * context = HIDGetDeviceContext(device);
  RtlZeroMemory(context, sizeof(*context));
  InitializeSRWLock(&context->reportLock);
  context->active = true;

  context->attributes.Size          =
    static_cast<ULONG>(sizeof(context->attributes));
  context->attributes.VendorID      = LG_INPUT_VENDOR_ID;
  context->attributes.ProductID     = LG_INPUT_PRODUCT_ID;
  context->attributes.VersionNumber = LG_INPUT_VERSION;

  context->descriptor.bLength                 =
    static_cast<UCHAR>(sizeof(context->descriptor));
  context->descriptor.bDescriptorType         = HID_HID_DESCRIPTOR_TYPE;
  context->descriptor.bcdHID                  = 0x0111;
  context->descriptor.bCountry                = 0;
  context->descriptor.bNumDescriptors         = 1;
  auto & reportDescriptor = context->descriptor.DescriptorList[0];
  reportDescriptor.bReportType    = HID_REPORT_DESCRIPTOR_TYPE;
  reportDescriptor.wReportLength =
    static_cast<USHORT>(HIDGetReportDescriptorSize());

  status = CreateQueues(device);
  if (!NT_SUCCESS(status))
    return status;

  {
    CSRWExclusiveLock lock(&s_deviceLock);
    s_device = context;
  }
  return STATUS_SUCCESS;
}

NTSTATUS CHIDDevice::SubmitReport(
  _In_reads_bytes_(size) const void * report,
  _In_ size_t size)
{
  if (!report || !size)
    return STATUS_INVALID_PARAMETER;

  const UCHAR reportId = *static_cast<const UCHAR *>(report);
  if (HIDGetInputReportSize(reportId) != size)
    return STATUS_INVALID_PARAMETER;

  CSRWSharedLock deviceLock(&s_deviceLock);
  HIDDeviceContext * context = s_device;
  if (!context)
    return STATUS_DEVICE_NOT_READY;

  WDFREQUEST request = nullptr;
  NTSTATUS   status;
  {
    CSRWExclusiveLock reportLock(&context->reportLock);
    if (context->stopping || !context->active)
      status = STATUS_DEVICE_NOT_READY;
    else
    {
      status = WdfIoQueueRetrieveNextRequest(context->reportQueue, &request);
      if (status == STATUS_NO_MORE_ENTRIES)
      {
        request = nullptr;
        status = QueueReport(context, report, size);
      }
    }
  }

  if (request)
  {
    status = CopyToRequest(request, report, size);
    WdfRequestComplete(request, status);
  }

  return status;
}

VOID HIDEvtIoDeviceControl(
  _In_ WDFQUEUE queue,
  _In_ WDFREQUEST request,
  _In_ size_t outputBufferLength,
  _In_ size_t inputBufferLength,
  _In_ ULONG ioControlCode)
{
  UNREFERENCED_PARAMETER(outputBufferLength);
  UNREFERENCED_PARAMETER(inputBufferLength);

  HIDDeviceContext * context =
    HIDGetDeviceContext(WdfIoQueueGetDevice(queue));
  NTSTATUS status;
  bool     complete = true;

  switch (ioControlCode)
  {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
      status = CopyToRequest(
        request, &context->descriptor, context->descriptor.bLength);
      break;

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
      status = CopyToRequest(
        request, &context->attributes, sizeof(context->attributes));
      break;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
      status = CopyToRequest(
        request, HIDGetReportDescriptor(), HIDGetReportDescriptorSize());
      break;

    case IOCTL_HID_READ_REPORT:
      status = ReadReport(context, request, &complete);
      break;

    case IOCTL_HID_WRITE_REPORT:
    case IOCTL_UMDF_HID_SET_OUTPUT_REPORT:
      status = SetOutputReport(queue, request);
      break;

    case IOCTL_HID_ACTIVATE_DEVICE:
      status = ActivateDevice(context);
      break;

    case IOCTL_HID_DEACTIVATE_DEVICE:
      status = DeactivateDevice(context);
      break;

    default:
      status = STATUS_NOT_SUPPORTED;
      break;
  }

  if (complete)
    WdfRequestComplete(request, status);
}

VOID HIDEvtReportQueueCleanup(_In_ WDFOBJECT object)
{
  HIDDeviceContext * context =
    HIDGetDeviceContext(WdfIoQueueGetDevice((WDFQUEUE)object));

  CSRWExclusiveLock deviceLock(&s_deviceLock);
  if (s_device == context)
    s_device = nullptr;

  {
    CSRWExclusiveLock reportLock(&context->reportLock);
    context->stopping    = true;
    context->reportHead  = 0;
    context->reportCount = 0;
  }
}
