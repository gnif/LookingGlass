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

#include "CClipboardChannel.h"
#include <LGProtocol/KVMFRClipboard.h>

#include <stdint.h>

class IClipboardTarget
{
public:
  virtual ~IClipboardTarget() = default;

  // Sends one client-to-Helper record. record.generation must be the binding
  // generation most recently supplied by ClipboardState; transports which
  // expose their own endpoint generation validate and replace it at this
  // adapter boundary. clipboardGeneration remains the clipboard content
  // generation and is preserved end-to-end.
  //
  // BUSY consumes neither the record nor its borrowed data; the source
  // retains and retries the exact record from its bounded worker/Process
  // drain.
  virtual ClipboardChannelResult SendClipboard(
    const KVMFRClipboardMessage& record, const uint8_t * data) = 0;

  // Sends an ordered prefix of client-to-Helper records. accepted reports
  // how many records were consumed for every result; the source retains and
  // retries an unaccepted suffix when the target returns BUSY.
  virtual ClipboardChannelResult SendClipboardBatch(
    const ClipboardChannelWrite * records, size_t count, size_t& accepted) = 0;

  // The source calls ClipboardReceiveReady after a Helper-to-client record
  // returned BUSY and it can accept an exact retry of that record.
  virtual void ClipboardReceiveReady() = 0;
  virtual void ClipboardFailed() = 0;
};

class IClipboardSource
{
public:
  virtual ~IClipboardSource() = default;

  // Start attaches the target, but the source must wait for the initial
  // ClipboardState callback before calling target.SendClipboard. Stop is a
  // callback-quiescence barrier and leaves no work which can access target;
  // it also cleans up a partially completed or failed Start.
  virtual bool Start(IClipboardTarget& target) = 0;
  virtual void Stop() = 0;

  // All callbacks into the source, including Start and Stop, are serialized.
  // record.generation is stamped with the current binding generation. A
  // generation change invalidates records from an earlier generation and
  // cancels their transfers; clipboardGeneration is unaffected. data is
  // borrowed only for the duration of SendClipboard. BUSY consumes neither
  // record nor data, and the channel retries the same record after
  // ClipboardReceiveReady (or its bounded polling fallback).
  virtual void ClipboardState(bool available, uint32_t generation) = 0;
  virtual ClipboardChannelResult SendClipboard(
    const KVMFRClipboardMessage& record, const uint8_t * data) = 0;

  // Reset advances generation before the callback. The source cancels all
  // transfers from the preceding generation.
  virtual void ClipboardReset(uint32_t generation, uint32_t reason) = 0;
};
