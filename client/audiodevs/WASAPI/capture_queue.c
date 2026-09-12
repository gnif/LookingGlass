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

#include "capture_queue.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

bool wasapi_captureQueueInit(WasapiCaptureQueue * queue, size_t capacity,
    size_t slotSize)
{
  if (!queue || !capacity || !slotSize || capacity == SIZE_MAX)
    return false;

  const size_t slotCount = capacity + 1;
  if (slotCount > SIZE_MAX / sizeof(*queue->packets) ||
      slotCount > SIZE_MAX / slotSize)
    return false;

  WasapiCapturePacket * packets = calloc(slotCount, sizeof(*packets));
  uint8_t             * data    = malloc(slotCount * slotSize);
  if (!packets || !data)
  {
    free(data);
    free(packets);
    return false;
  }

  for (size_t i = 0; i < slotCount; ++i)
    packets[i].data = data + i * slotSize;

  queue->packets   = packets;
  queue->data      = data;
  queue->slotSize  = slotSize;
  queue->slotCount = slotCount;
  atomic_init(&queue->head, 0);
  atomic_init(&queue->tail, 0);
  return true;
}

void wasapi_captureQueueFree(WasapiCaptureQueue * queue)
{
  if (!queue)
    return;

  free(queue->data);
  free(queue->packets);
  *queue = (WasapiCaptureQueue) { 0 };
}

void wasapi_captureQueueReset(WasapiCaptureQueue * queue)
{
  if (!queue)
    return;

  atomic_store_explicit(&queue->head, 0, memory_order_relaxed);
  atomic_store_explicit(&queue->tail, 0, memory_order_relaxed);
}

bool wasapi_captureQueuePush(WasapiCaptureQueue * queue,
    const uint8_t * data, size_t bytes, int frames,
    const LG_AudioClock * clock)
{
  if (!data || frames < 0)
    return false;

  uint8_t * destination = wasapi_captureQueueBegin(queue, bytes);
  if (!destination)
    return false;

  memcpy(destination, data, bytes);
  wasapi_captureQueueCommit(queue, bytes, frames, clock);
  return true;
}

uint8_t * wasapi_captureQueueBegin(WasapiCaptureQueue * queue, size_t bytes)
{
  if (!queue || !queue->packets || bytes > queue->slotSize)
    return NULL;

  const size_t head = atomic_load_explicit(
      &queue->head, memory_order_relaxed);
  const size_t next = (head + 1) % queue->slotCount;
  if (next == atomic_load_explicit(&queue->tail, memory_order_acquire))
    return NULL;

  return queue->packets[head].data;
}

void wasapi_captureQueueCommit(WasapiCaptureQueue * queue, size_t bytes,
    int frames, const LG_AudioClock * clock)
{
  const size_t head = atomic_load_explicit(
      &queue->head, memory_order_relaxed);
  WasapiCapturePacket * packet = &queue->packets[head];
  packet->bytes      = bytes;
  packet->frames     = frames;
  packet->clockValid = clock != NULL;
  packet->clock      = clock ? *clock : (LG_AudioClock) { 0 };
  atomic_store_explicit(&queue->head,
      (head + 1) % queue->slotCount, memory_order_release);
}

const WasapiCapturePacket * wasapi_captureQueueFront(
    WasapiCaptureQueue * queue)
{
  if (!queue || !queue->packets)
    return NULL;

  const size_t tail = atomic_load_explicit(
      &queue->tail, memory_order_relaxed);
  if (tail == atomic_load_explicit(&queue->head, memory_order_acquire))
    return NULL;

  return &queue->packets[tail];
}

void wasapi_captureQueuePop(WasapiCaptureQueue * queue)
{
  if (!queue || !queue->packets)
    return;

  const size_t tail = atomic_load_explicit(
      &queue->tail, memory_order_relaxed);
  atomic_store_explicit(
      &queue->tail, (tail + 1) % queue->slotCount, memory_order_release);
}
