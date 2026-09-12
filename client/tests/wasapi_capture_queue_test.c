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
#include <stdio.h>
#include <string.h>

#define TEST_ASSERT(expr) \
  do \
  { \
    if (!(expr)) \
    { \
      fprintf(stderr, "%s:%d: assertion failed: %s\n", \
          __FILE__, __LINE__, #expr); \
      return 1; \
    } \
  } \
  while (0)

static int testQueue(void)
{
  WasapiCaptureQueue queue = { 0 };
  TEST_ASSERT(!wasapi_captureQueueInit(NULL, 2, 4));
  TEST_ASSERT(!wasapi_captureQueueInit(&queue, 0, 4));
  TEST_ASSERT(!wasapi_captureQueueInit(&queue, 2, 0));
  TEST_ASSERT(wasapi_captureQueueInit(&queue, 2, 4));

  const uint8_t first[]  = { 1, 2, 3, 4 };
  const uint8_t second[] = { 5, 6, 7, 8 };
  const uint8_t third[]  = { 9, 10, 11, 12 };
  const LG_AudioClock clock =
  {
    .position      = 42,
    .time          = 1000,
    .rate          = 0.0,
    .stable        = true,
    .discontinuity = false
  };

  TEST_ASSERT(!wasapi_captureQueueBegin(&queue, sizeof(first) + 1));
  TEST_ASSERT(!wasapi_captureQueuePush(
      &queue, first, sizeof(first) + 1, 2, &clock));
  TEST_ASSERT(!wasapi_captureQueuePush(
      &queue, NULL, sizeof(first), 2, &clock));

  uint8_t * destination = wasapi_captureQueueBegin(&queue, sizeof(first));
  TEST_ASSERT(destination);
  memcpy(destination, first, sizeof(first));
  TEST_ASSERT(!wasapi_captureQueueFront(&queue));
  wasapi_captureQueueCommit(&queue, sizeof(first), 2, &clock);

  TEST_ASSERT(wasapi_captureQueuePush(
      &queue, second, sizeof(second), 2, NULL));
  TEST_ASSERT(!wasapi_captureQueuePush(
      &queue, third, sizeof(third), 2, &clock));

  const WasapiCapturePacket * packet = wasapi_captureQueueFront(&queue);
  TEST_ASSERT(packet);
  TEST_ASSERT(packet->bytes == sizeof(first));
  TEST_ASSERT(packet->frames == 2);
  TEST_ASSERT(packet->clockValid);
  TEST_ASSERT(packet->clock.position == clock.position);
  TEST_ASSERT(packet->clock.time == clock.time);
  TEST_ASSERT(packet->clock.rate == 0.0);
  TEST_ASSERT(packet->clock.stable);
  TEST_ASSERT(!packet->clock.discontinuity);
  TEST_ASSERT(memcmp(packet->data, first, sizeof(first)) == 0);
  wasapi_captureQueuePop(&queue);

  TEST_ASSERT(wasapi_captureQueuePush(
      &queue, third, sizeof(third), 2, &clock));
  packet = wasapi_captureQueueFront(&queue);
  TEST_ASSERT(packet);
  TEST_ASSERT(!packet->clockValid);
  TEST_ASSERT(memcmp(packet->data, second, sizeof(second)) == 0);
  wasapi_captureQueuePop(&queue);

  packet = wasapi_captureQueueFront(&queue);
  TEST_ASSERT(packet);
  TEST_ASSERT(packet->clockValid);
  TEST_ASSERT(memcmp(packet->data, third, sizeof(third)) == 0);
  wasapi_captureQueuePop(&queue);
  TEST_ASSERT(!wasapi_captureQueueFront(&queue));

  TEST_ASSERT(wasapi_captureQueuePush(
      &queue, first, sizeof(first), 2, &clock));
  wasapi_captureQueueReset(&queue);
  TEST_ASSERT(!wasapi_captureQueueFront(&queue));
  TEST_ASSERT(wasapi_captureQueuePush(
      &queue, second, sizeof(second), 2, NULL));

  wasapi_captureQueueFree(&queue);
  TEST_ASSERT(!queue.packets);
  TEST_ASSERT(!queue.data);
  wasapi_captureQueueFree(&queue);
  return 0;
}

int main(void)
{
  return testQueue();
}
