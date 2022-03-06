/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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

#include "common/ringbuffer.h"
#include "common/debug.h"
#include "common/util.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct RingBuffer
{
  uint32_t          length;
  uint32_t          valueSize;
  _Atomic(uint32_t) readPos;
  _Atomic(uint32_t) writePos;
  bool              unbounded;
  char              values[0];
};

RingBuffer ringbuffer_newInternal(int length, size_t valueSize,
    bool unbounded)
{
  DEBUG_ASSERT(valueSize > 0 && valueSize < UINT32_MAX);

  struct RingBuffer * rb = calloc(1, sizeof(*rb) + valueSize * length);
  if (!rb)
  {
    DEBUG_ERROR("out of memory");
    return NULL;
  }

  rb->length    = length;
  rb->valueSize = valueSize;
  atomic_store(&rb->readPos , 0);
  atomic_store(&rb->writePos, 0);
  rb->unbounded = unbounded;
  return rb;
}

RingBuffer ringbuffer_new(int length, size_t valueSize)
{
  return ringbuffer_newInternal(length, valueSize, false);
}

RingBuffer ringbuffer_newUnbounded(int length, size_t valueSize)
{
  return ringbuffer_newInternal(length, valueSize, true);
}

void ringbuffer_free(RingBuffer * rb)
{
  if (!*rb)
    return;

  free(*rb);
  *rb = NULL;
}

void ringbuffer_push(RingBuffer rb, const void * value)
{
  if (!rb->unbounded && ringbuffer_getCount(rb) == rb->length)
    ringbuffer_consume(rb, NULL, 1);

  ringbuffer_append(rb, value, 1);
}

void ringbuffer_reset(RingBuffer rb)
{
  atomic_store(&rb->readPos,  0);
  atomic_store(&rb->writePos, 0);
}

int ringbuffer_getLength(const RingBuffer rb)
{
  return rb->length;
}

int ringbuffer_getStart(const RingBuffer rb)
{
  return atomic_load(&rb->readPos) % rb->length;
}

int ringbuffer_getCount(const RingBuffer rb)
{
  uint32_t writePos = atomic_load(&rb->writePos);
  uint32_t readPos  = atomic_load(&rb->readPos);

  return writePos - readPos;
}

void * ringbuffer_getValues(const RingBuffer rb)
{
  return rb->values;
}

int ringbuffer_append(const RingBuffer rb, const void * values, int count)
{
  if (count == 0)
    return 0;

  // Seeking backwards is only supported in unbounded mode at the moment
  if (count < 0 && !rb->unbounded)
    return 0;

  uint32_t readPos = atomic_load_explicit(&rb->readPos, memory_order_acquire);
  uint32_t writePos = atomic_load_explicit(&rb->writePos, memory_order_relaxed);
  uint32_t newWritePos = writePos;

  if (count < 0)
  {
    // Seeking backwards; just update the write pointer
    newWritePos += count;
  }
  else
  {
    int32_t writeOffset = writePos - readPos;
    if (writeOffset < 0)
    {
      DEBUG_ASSERT(rb->unbounded);

      // The reader is ahead of the writer; skip new values to remain in sync
      int32_t underrun = -writeOffset;
      int32_t skipLen = min(underrun, count);

      if (values)
        values += skipLen * rb->valueSize;

      count       -= skipLen;
      newWritePos += skipLen;
      writeOffset  = newWritePos - readPos;
    }

    if (count > 0)
    {
      DEBUG_ASSERT(writeOffset >= 0);

      // We may not be able to write anything if the writer is too far ahead of
      // the reader
      uint32_t writeLen = 0;
      if (writeOffset < rb->length) {
        uint32_t writeIndex = newWritePos % rb->length;
        uint32_t writeAvailable = rb->length - writeOffset;
        uint32_t writeAvailableBack =
          min(rb->length - writeIndex, writeAvailable);

        writeLen = min(count, writeAvailable);
        uint32_t writeLenBack = min(writeLen, writeAvailableBack);
        uint32_t writeLenFront = writeLen - writeLenBack;

        if (values)
        {
          memcpy(rb->values + writeIndex * rb->valueSize, values,
            writeLenBack * rb->valueSize);
          memcpy(rb->values, values + writeLenBack * rb->valueSize,
            writeLenFront * rb->valueSize);
        }
        else
        {
          memset(rb->values + writeIndex * rb->valueSize, 0,
            writeLenBack * rb->valueSize);
          memset(rb->values, 0, writeLenFront * rb->valueSize);
        }
      }

      if (rb->unbounded)
        newWritePos += count;
      else
        newWritePos += writeLen;
    }
  }

  atomic_store_explicit(&rb->writePos, newWritePos, memory_order_release);

  return newWritePos - writePos;
}

int ringbuffer_consume(const RingBuffer rb, void * values, int count)
{
  if (count == 0)
    return 0;

  // Seeking backwards is only supported in unbounded mode at the moment
  if (count < 0 && !rb->unbounded)
    return 0;

  uint32_t readPos = atomic_load_explicit(&rb->readPos, memory_order_relaxed);
  uint32_t writePos = atomic_load_explicit(&rb->writePos, memory_order_acquire);
  uint32_t newReadPos = readPos;

  if (count < 0)
  {
    // Seeking backwards; just update the read pointer
    newReadPos += count;
  }
  else
  {
    int32_t writeOffset = writePos - newReadPos;
    if (writeOffset < 0)
    {
      DEBUG_ASSERT(rb->unbounded);

      // We are already in an underrun condition; just fill the buffer with
      // zeros
      newReadPos += count;

      if (values)
        memset(values, 0, count * rb->valueSize);
    }
    else
    {
      uint32_t readIndex = newReadPos % rb->length;
      uint32_t readAvailable = min(writeOffset, rb->length);
      uint32_t readLen = min(count, readAvailable);

      if (values)
      {
        uint32_t readAvailableBack = min(rb->length - readIndex, readAvailable);
        uint32_t readLenBack = min(readLen, readAvailableBack);
        uint32_t readLenFront = readLen - readLenBack;

        memcpy(values, rb->values + readIndex * rb->valueSize,
               readLenBack * rb->valueSize);
        memcpy(values + readLenBack * rb->valueSize, rb->values,
               readLenFront * rb->valueSize);

        if (rb->unbounded && readLen < count)
        {
          // One of two things has happened: we have caught up with the writer
          // and are starting to underrun, or we are really far behind the
          // writer and an overrun has occurred. Either way, the only thing left
          // to do is to fill the rest of the buffer with zeros
          uint32_t remaining = count - readLen;
          memset(values + readLen * rb->valueSize, 0,
            remaining * rb->valueSize);
        }
      }

      if (rb->unbounded)
        newReadPos += count;
      else
        newReadPos += readLen;
    }
  }

  atomic_store_explicit(&rb->readPos, newReadPos, memory_order_release);

  return newReadPos - readPos;
}

void ringbuffer_forEach(const RingBuffer rb, RingBufferIterator fn,
    void * udata, bool reverse)
{
  uint32_t readPos = atomic_load_explicit(&rb->readPos, memory_order_relaxed);
  uint32_t writePos = atomic_load_explicit(&rb->writePos, memory_order_acquire);

  int32_t writeOffset = writePos - readPos;
  if (writeOffset < 0)
  {
    DEBUG_ASSERT(rb->unbounded);
    return;
  }

  uint32_t readAvailable = min(writeOffset, rb->length);

  if (reverse)
  {
    readPos = readPos + readAvailable - 1;
    for (int i = 0; i < readAvailable; ++i, --readPos)
    {
      uint32_t readIndex = readPos % rb->length;
      void * value = rb->values + readIndex * rb->valueSize;
      if (!fn(i, value, udata))
        break;
    }
  }
  else
  {
    for (int i = 0; i < readAvailable; ++i, ++readPos)
    {
      uint32_t readIndex = readPos % rb->length;
      void * value = rb->values + readIndex * rb->valueSize;
      if (!fn(i, value, udata))
        break;
    }
  }
}
