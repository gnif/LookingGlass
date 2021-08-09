/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

#include <stdlib.h>
#include <string.h>

struct RingBuffer
{
  RingBufferValueFn preOverwriteFn;
  void *            preOverwriteUdata;

  int     length;
  size_t  valueSize;
  int     start, pos, count;
  char    values[0];
};

RingBuffer ringbuffer_new(int length, size_t valueSize)
{
  struct RingBuffer * rb = calloc(1, sizeof(*rb) + valueSize * length);
  rb->length    = length;
  rb->valueSize = valueSize;
  return rb;
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
  void * dst = rb->values + rb->pos * rb->valueSize;

  if (rb->count < rb->length)
    ++rb->count;
  else
  {
    if (++rb->start == rb->length)
      rb->start = 0;

    if (rb->preOverwriteFn)
      rb->preOverwriteFn(dst, rb->preOverwriteUdata);
  }

  memcpy(dst, value, rb->valueSize);

  if (++rb->pos == rb->length)
    rb->pos = 0;
}

void ringbuffer_reset(RingBuffer rb)
{
  rb->start = 0;
  rb->pos   = 0;
  rb->count = 0;
}

int ringbuffer_getLength(const RingBuffer rb)
{
  return rb->length;
}

int ringbuffer_getStart(const RingBuffer rb)
{
  return rb->start;
}

int ringbuffer_getCount(const RingBuffer rb)
{
  return rb->count;
}

void * ringbuffer_getValues(const RingBuffer rb)
{
  return rb->values;
}

void * ringBuffer_getLastValue(const RingBuffer rb)
{
  if (rb->count == 0)
    return NULL;

  int index = rb->start + rb->count - 1;
  if (index >= rb->length)
    index -= rb->length;

  return rb->values + index * rb->valueSize;
}

void ringbuffer_setPreOverwriteFn(const RingBuffer rb, RingBufferValueFn fn,
    void * udata)
{
  rb->preOverwriteFn    = fn;
  rb->preOverwriteUdata = udata;
}

void ringbuffer_forEach(const RingBuffer rb, RingBufferIterator fn, void * udata,
    bool reverse)
{
  if (reverse)
  {
    int index = rb->start + rb->count - 1;
    if (index >= rb->length)
      index -= rb->length;

    for(int i = 0; i < rb->count; ++i)
    {
      void * value = rb->values + index * rb->valueSize;
      if (--index == -1)
        index = rb->length - 1;

      if (!fn(i, value, udata))
        break;
    }
  }
  else
  {
    int index = rb->start;
    for(int i = 0; i < rb->count; ++i)
    {
      void * value = rb->values + index * rb->valueSize;
      if (++index == rb->length)
        index = 0;

      if (!fn(i, value, udata))
        break;
    }
  }
}
