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

#include "transport/lgmp/CLGMPClipboardFiles.h"

bool CLGMPClipboardFiles::FromHelper(Direction direction)
{
  return direction == Direction::HELPER_TO_CLIENT;
}

bool CLGMPClipboardFiles::RequesterHelper(uint64_t transfer)
{
  return kvmfrClipboardTransferFromHelper(transfer) != 0;
}

bool CLGMPClipboardFiles::SameRequester(
  Direction direction, uint64_t transfer)
{
  return FromHelper(direction) == RequesterHelper(transfer);
}

CLGMPClipboardFiles::Acquisition *
CLGMPClipboardFiles::FindAcquisition(uint64_t transfer)
{
  for (Acquisition& acquisition : m_acquisitions)
    if (acquisition.transfer == transfer)
      return &acquisition;
  return nullptr;
}

const CLGMPClipboardFiles::Acquisition *
CLGMPClipboardFiles::FindAcquisition(uint64_t transfer) const
{
  for (const Acquisition& acquisition : m_acquisitions)
    if (acquisition.transfer == transfer)
      return &acquisition;
  return nullptr;
}

const CLGMPClipboardFiles::Acquisition *
CLGMPClipboardFiles::FindAcquired(
  uint64_t dataset, bool requesterHelper) const
{
  for (const Acquisition& acquisition : m_acquisitions)
    if (acquisition.dataset == dataset && acquisition.requesterHelper ==
        requesterHelper && acquisition.acquired)
      return &acquisition;
  return nullptr;
}

bool CLGMPClipboardFiles::HasAcquisition(
  uint64_t dataset, bool requesterHelper) const
{
  for (const Acquisition& acquisition : m_acquisitions)
    if (acquisition.dataset == dataset &&
        acquisition.requesterHelper == requesterHelper)
      return true;
  return false;
}

CLGMPClipboardFiles::Acquisition * CLGMPClipboardFiles::FreeAcquisition()
{
  for (Acquisition& acquisition : m_acquisitions)
    if (!acquisition.Active())
      return &acquisition;
  return nullptr;
}

bool CLGMPClipboardFiles::HasFreeAcquisition() const
{
  for (const Acquisition& acquisition : m_acquisitions)
    if (!acquisition.Active())
      return true;
  return false;
}

CLGMPClipboardFiles::Request * CLGMPClipboardFiles::FindRequest(
  uint64_t transfer)
{
  for (Request& request : m_requests)
    if (request.transfer == transfer)
      return &request;
  return nullptr;
}

const CLGMPClipboardFiles::Request * CLGMPClipboardFiles::FindRequest(
  uint64_t transfer) const
{
  for (const Request& request : m_requests)
    if (request.transfer == transfer)
      return &request;
  return nullptr;
}

CLGMPClipboardFiles::Request * CLGMPClipboardFiles::FreeRequest()
{
  for (Request& request : m_requests)
    if (!request.Active())
      return &request;
  return nullptr;
}

bool CLGMPClipboardFiles::HasFreeRequest() const
{
  for (const Request& request : m_requests)
    if (!request.Active())
      return true;
  return false;
}

bool CLGMPClipboardFiles::TransferInUse(uint64_t transfer) const
{
  return FindAcquisition(transfer) || FindRequest(transfer);
}

bool CLGMPClipboardFiles::ValidateData(const Request& request,
  const KVMFRClipboardMessage& message) const
{
  if (message.clipboardGeneration != request.dataset ||
      message.token != request.operation ||
      message.offset != request.expectedOffset ||
      message.sequence != request.expectedSequence)
    return false;

  const uint64_t end = message.offset + message.length;
  if (!request.began)
  {
    if (message.offset || message.sequence ||
        !(message.flags & KVMFR_CLIPBOARD_FLAG_BEGIN))
      return false;
  }
  else if (message.flags & KVMFR_CLIPBOARD_FLAG_BEGIN)
    return false;

  const uint64_t hint = request.began ? request.sizeHint : message.size;
  if (hint != KVMFR_CLIPBOARD_SIZE_UNKNOWN && end > hint)
    return false;
  if (request.operation == KVMFR_CLIPBOARD_FILE_OP_READ &&
      end > request.requestedBytes)
    return false;
  if (message.flags & KVMFR_CLIPBOARD_FLAG_END)
    return message.size == end &&
      (hint == KVMFR_CLIPBOARD_SIZE_UNKNOWN || hint == end);
  return true;
}

void CLGMPClipboardFiles::RemoveAcquisition(Acquisition& acquisition)
{
  const uint64_t dataset = acquisition.dataset;
  const bool requesterHelper = acquisition.requesterHelper;
  acquisition.Clear();
  for (Request& request : m_requests)
    if (request.dataset == dataset &&
        request.requesterHelper == requesterHelper)
      request.Clear();
}

bool CLGMPClipboardFiles::IsRecord(const KVMFRClipboardMessage& message)
{
  return kvmfrClipboardFileMessageType(message.type) != 0;
}

bool CLGMPClipboardFiles::IsData(const KVMFRClipboardMessage& message)
{
  return message.type == KVMFR_CLIPBOARD_MESSAGE_FILE_DATA;
}

bool CLGMPClipboardFiles::DirectionValid(
  const KVMFRClipboardMessage& message, Direction direction)
{
  if (!kvmfrClipboardFileMessageValid(&message))
    return false;

  switch (message.type)
  {
    case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE:
    case KVMFR_CLIPBOARD_MESSAGE_FILE_RELEASE:
    case KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST:
      return SameRequester(direction, message.transfer);

    case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED:
    case KVMFR_CLIPBOARD_MESSAGE_FILE_DATA:
      return !SameRequester(direction, message.transfer);

    case KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL:
      return true;

    default:
      return false;
  }
}

bool CLGMPClipboardFiles::TransferActive(uint64_t transfer) const
{
  return TransferInUse(transfer);
}

bool CLGMPClipboardFiles::IsStaleTerminal(
  const KVMFRClipboardMessage& message, Direction direction) const
{
  if (!DirectionValid(message, direction) ||
      TransferInUse(message.transfer))
    return false;

  switch (message.type)
  {
    case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED:
    case KVMFR_CLIPBOARD_MESSAGE_FILE_DATA:
      return !SameRequester(direction, message.transfer);

    case KVMFR_CLIPBOARD_MESSAGE_FILE_RELEASE:
      return SameRequester(direction, message.transfer);

    case KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL:
      return true;

    default:
      return false;
  }
}

bool CLGMPClipboardFiles::Validate(const KVMFRClipboardMessage& message,
  Direction direction, uint64_t offeredDataset, bool filesOffered) const
{
  if (!DirectionValid(message, direction))
    return false;

  switch (message.type)
  {
    case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE:
      return SameRequester(direction, message.transfer) && filesOffered &&
        message.clipboardGeneration == offeredDataset &&
        !TransferInUse(message.transfer) &&
        !HasAcquisition(message.clipboardGeneration,
          FromHelper(direction)) && HasFreeAcquisition();

    case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED:
    {
      const Acquisition * acquisition =
        FindAcquisition(message.transfer);
      return acquisition && !acquisition->acquired &&
        acquisition->dataset == message.clipboardGeneration &&
        FromHelper(direction) != acquisition->requesterHelper;
    }

    case KVMFR_CLIPBOARD_MESSAGE_FILE_RELEASE:
    {
      const Acquisition * acquisition =
        FindAcquisition(message.transfer);
      return acquisition && acquisition->acquired &&
        acquisition->dataset == message.clipboardGeneration &&
        FromHelper(direction) == acquisition->requesterHelper;
    }

    case KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST:
      return SameRequester(direction, message.transfer) &&
        !TransferInUse(message.transfer) && HasFreeRequest() &&
        FindAcquired(message.clipboardGeneration,
          FromHelper(direction));

    case KVMFR_CLIPBOARD_MESSAGE_FILE_DATA:
    {
      const Request * request = FindRequest(message.transfer);
      return request &&
        FromHelper(direction) != request->requesterHelper &&
        ValidateData(*request, message);
    }

    case KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL:
    {
      const Acquisition * acquisition =
        FindAcquisition(message.transfer);
      if (acquisition)
        return acquisition->dataset == message.clipboardGeneration;
      const Request * request = FindRequest(message.transfer);
      return request && request->dataset == message.clipboardGeneration;
    }

    default:
      return false;
  }
}

void CLGMPClipboardFiles::Apply(
  const KVMFRClipboardMessage& message, Direction direction)
{
  switch (message.type)
  {
    case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE:
    {
      Acquisition * acquisition = FreeAcquisition();
      if (!acquisition)
        return;
      acquisition->dataset = message.clipboardGeneration;
      acquisition->transfer = message.transfer;
      acquisition->requesterHelper = FromHelper(direction);
      acquisition->acquired = false;
      break;
    }

    case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED:
    {
      Acquisition * acquisition = FindAcquisition(message.transfer);
      if (!acquisition)
        return;
      if (message.token == KVMFR_CLIPBOARD_FILE_ERROR_NONE)
        acquisition->acquired = true;
      else
        RemoveAcquisition(*acquisition);
      break;
    }

    case KVMFR_CLIPBOARD_MESSAGE_FILE_RELEASE:
    case KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL:
    {
      Acquisition * acquisition = FindAcquisition(message.transfer);
      if (acquisition)
      {
        RemoveAcquisition(*acquisition);
        break;
      }
      Request * request = FindRequest(message.transfer);
      if (request)
        request->Clear();
      break;
    }

    case KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST:
    {
      Request * request = FreeRequest();
      if (!request)
        return;
      request->dataset = message.clipboardGeneration;
      request->transfer = message.transfer;
      request->operation = message.token;
      request->requestedBytes = message.token ==
        KVMFR_CLIPBOARD_FILE_OP_READ ? message.flags : 0;
      request->requesterHelper = FromHelper(direction);
      request->sizeHint = KVMFR_CLIPBOARD_SIZE_UNKNOWN;
      break;
    }

    case KVMFR_CLIPBOARD_MESSAGE_FILE_DATA:
    {
      Request * request = FindRequest(message.transfer);
      if (!request)
        return;
      if (!request->began)
      {
        request->began = true;
        request->sizeHint = message.size;
      }
      request->expectedOffset += message.length;
      ++request->expectedSequence;
      if (message.flags & KVMFR_CLIPBOARD_FLAG_END)
        request->Clear();
      break;
    }
  }
}

void CLGMPClipboardFiles::Reset()
{
  for (Acquisition& acquisition : m_acquisitions)
    acquisition.Clear();
  for (Request& request : m_requests)
    request.Clear();
}

unsigned CLGMPClipboardFiles::Disconnect(KVMFRClipboardMessage * records,
  unsigned capacity, uint32_t endpointGeneration)
{
  unsigned count = 0;
  for (const Request& request : m_requests)
  {
    if (!request.Active() || count == capacity)
      continue;
    KVMFRClipboardMessage& record = records[count++];
    record = {};
    record.version = KVMFR_CLIPBOARD_VERSION;
    record.type = KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL;
    record.generation = endpointGeneration;
    record.clipboardGeneration = request.dataset;
    record.transfer = request.transfer;
    record.format = KVMFR_CLIPBOARD_FORMAT_FILES;
    record.token = request.requesterHelper ?
      KVMFR_CLIPBOARD_FILE_ERROR_DISCONNECTED :
      KVMFR_CLIPBOARD_FILE_ERROR_CANCELLED;
  }

  for (const Acquisition& acquisition : m_acquisitions)
  {
    if (!acquisition.Active() || count == capacity)
      continue;
    KVMFRClipboardMessage& record = records[count++];
    record = {};
    record.version = KVMFR_CLIPBOARD_VERSION;
    record.type = acquisition.requesterHelper ?
      (acquisition.acquired ? KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL :
        KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED) :
      (acquisition.acquired ? KVMFR_CLIPBOARD_MESSAGE_FILE_RELEASE :
        KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL);
    record.generation = endpointGeneration;
    record.clipboardGeneration = acquisition.dataset;
    record.transfer = acquisition.transfer;
    record.format = KVMFR_CLIPBOARD_FORMAT_FILES;
    if (acquisition.requesterHelper)
      record.token = KVMFR_CLIPBOARD_FILE_ERROR_DISCONNECTED;
    else if (!acquisition.acquired)
      record.token = KVMFR_CLIPBOARD_FILE_ERROR_CANCELLED;
  }
  Reset();
  return count;
}
