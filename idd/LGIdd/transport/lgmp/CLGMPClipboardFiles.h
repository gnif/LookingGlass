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

#include <LGProtocol/KVMFRClipboard.h>

#include <stdint.h>

class CLGMPClipboardFiles
{
public:
  enum class Direction
  {
    CLIENT_TO_HELPER,
    HELPER_TO_CLIENT,
  };

  static constexpr unsigned MAX_ACQUISITIONS =
    KVMFR_CLIPBOARD_FILE_MAX_ACQUISITIONS;
  static constexpr unsigned MAX_REQUESTS =
    KVMFR_CLIPBOARD_FILE_MAX_REQUESTS;

private:
  struct Acquisition
  {
    uint64_t dataset = 0;
    uint64_t transfer = 0;
    bool requesterHelper = false;
    bool acquired = false;

    bool Active() const { return transfer != 0; }
    void Clear() { *this = {}; }
  };

  struct Request
  {
    uint64_t dataset = 0;
    uint64_t transfer = 0;
    uint64_t expectedOffset = 0;
    uint64_t sizeHint = KVMFR_CLIPBOARD_SIZE_UNKNOWN;
    uint32_t expectedSequence = 0;
    uint32_t requestedBytes = 0;
    KVMFRClipboardFileOperation operation = 0;
    bool requesterHelper = false;
    bool began = false;

    bool Active() const { return transfer != 0; }
    void Clear() { *this = {}; }
  };

  Acquisition m_acquisitions[MAX_ACQUISITIONS] = {};
  Request m_requests[MAX_REQUESTS] = {};

  static bool FromHelper(Direction direction);
  static bool RequesterHelper(uint64_t transfer);
  static bool SameRequester(Direction direction, uint64_t transfer);

  Acquisition * FindAcquisition(uint64_t transfer);
  const Acquisition * FindAcquisition(uint64_t transfer) const;
  const Acquisition * FindAcquired(
    uint64_t dataset, bool requesterHelper) const;
  bool HasAcquisition(uint64_t dataset, bool requesterHelper) const;
  Acquisition * FreeAcquisition();
  bool HasFreeAcquisition() const;

  Request * FindRequest(uint64_t transfer);
  const Request * FindRequest(uint64_t transfer) const;
  Request * FreeRequest();
  bool HasFreeRequest() const;

  bool TransferInUse(uint64_t transfer) const;
  bool ValidateData(const Request& request,
    const KVMFRClipboardMessage& message) const;
  void RemoveAcquisition(Acquisition& acquisition);

public:
  static bool IsRecord(const KVMFRClipboardMessage& message);
  static bool IsData(const KVMFRClipboardMessage& message);
  static bool DirectionValid(const KVMFRClipboardMessage& message,
    Direction direction);
  bool TransferActive(uint64_t transfer) const;
  bool IsStaleTerminal(const KVMFRClipboardMessage& message,
    Direction direction) const;

  bool Validate(const KVMFRClipboardMessage& message,
    Direction direction, uint64_t offeredDataset,
    bool filesOffered) const;
  void Apply(const KVMFRClipboardMessage& message, Direction direction);
  void Reset();

  // Builds the records which release Helper-owned resources or fail Helper
  // requests when an LGMP clipboard owner disappears. The state is reset even
  // when capacity is smaller than the number of generated records.
  unsigned Disconnect(KVMFRClipboardMessage * records, unsigned capacity,
    uint32_t endpointGeneration);
};
