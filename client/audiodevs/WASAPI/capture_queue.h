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

#ifndef _H_LG_WASAPI_CAPTURE_QUEUE_
#define _H_LG_WASAPI_CAPTURE_QUEUE_

#include "interface/audio.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct WasapiCapturePacket
{
  uint8_t      * data;
  size_t         bytes;
  int            frames;
  bool           clockValid;
  LG_AudioClock  clock;
}
WasapiCapturePacket;

typedef struct WasapiCaptureQueue
{
  WasapiCapturePacket * packets;
  uint8_t             * data;
  size_t                slotSize;
  size_t                slotCount;
  _Atomic(size_t)       head;
  _Atomic(size_t)       tail;
}
WasapiCaptureQueue;

bool                        wasapi_captureQueueInit   (
    WasapiCaptureQueue * queue, size_t capacity,
    size_t slotSize);
void                        wasapi_captureQueueFree   (
    WasapiCaptureQueue * queue);
void                        wasapi_captureQueueReset  (
    WasapiCaptureQueue * queue);
uint8_t                   * wasapi_captureQueueBegin  (
    WasapiCaptureQueue * queue, size_t bytes);
void                        wasapi_captureQueueCommit (
    WasapiCaptureQueue * queue, size_t bytes, int frames,
    const LG_AudioClock * clock);
bool                        wasapi_captureQueuePush   (
    WasapiCaptureQueue * queue,
    const uint8_t * data, size_t bytes, int frames,
    const LG_AudioClock * clock);
const WasapiCapturePacket * wasapi_captureQueueFront  (
    WasapiCaptureQueue * queue);
void                        wasapi_captureQueuePop    (
    WasapiCaptureQueue * queue);

#endif
