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

#include <stddef.h>
#include <stdbool.h>

typedef struct RingBuffer * RingBuffer;

RingBuffer ringbuffer_new(int length, size_t valueSize);

void ringbuffer_free(RingBuffer * rb);
void ringbuffer_push(RingBuffer rb, const void * value);
void ringbuffer_reset(RingBuffer rb);

int    ringbuffer_getLength(const RingBuffer rb);
int    ringbuffer_getStart (const RingBuffer rb);
int    ringbuffer_getCount (const RingBuffer rb);
void * ringbuffer_getValues(const RingBuffer rb);
void * ringBuffer_getLastValue(const RingBuffer rb);

typedef void (*RingBufferValueFn)(void * value, void * udata);

// set a function to call before a value is about to be overwritten
void ringbuffer_setPreOverwriteFn(RingBuffer rb, RingBufferValueFn fn,
    void * udata);

typedef bool (*RingBufferIterator)(int index, void * value, void * udata);
void ringbuffer_forEach(const RingBuffer rb, RingBufferIterator fn,
    void * udata, bool reverse);
