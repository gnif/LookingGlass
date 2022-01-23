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

#include <stddef.h>
#include <stdbool.h>

typedef struct RingBuffer * RingBuffer;

RingBuffer ringbuffer_new(int length, size_t valueSize);

/* In an unbounded ring buffer, the read and write pointers are free to move
 * independently of one another. This is useful if your input and output streams
 * are progressing at the same rate on average, and you want to keep the
 * latency stable in the event than an underrun or overrun occurs.
 *
 * If an underrun occurs (i.e., there is not enough data in the buffer to
 * satisfy a read request), the missing values with be filled with zeros. When
 * the writer catches up, the same number of values will be skipped from the
 * input.
 *
 * If an overrun occurs (i.e., there is not enough free space in the buffer to
 * satisfy a write request), excess values will be discarded. When the reader
 * catches up, the same number of values will be zeroed in the output.
 */
RingBuffer ringbuffer_newUnbounded(int length, size_t valueSize);

void ringbuffer_free(RingBuffer * rb);
void ringbuffer_push(RingBuffer rb, const void * value);
void ringbuffer_reset(RingBuffer rb);

/* Note that the following functions are NOT thread-safe */
int    ringbuffer_getLength(const RingBuffer rb);
int    ringbuffer_getStart (const RingBuffer rb);
int    ringbuffer_getCount (const RingBuffer rb);
void * ringbuffer_getValues(const RingBuffer rb);

/* Appends up to count values to the buffer returning the number of values
 * appended. If the buffer is unbounded, the return value is always count;
 * excess values will be discarded if the buffer is full. Pass a null values
 * pointer to write zeros to the buffer. Count may be negative in unbounded mode
 * to seek backwards.
 * Note: This function is thread-safe */
int ringbuffer_append(const RingBuffer rb, const void * values, int count);

/* Consumes up to count values from the buffer returning the number of values
 * consumed. If the buffer is unbounded, the return value is always count;
 * excess values will be zeroed if there is not enough data in the buffer. Pass
 * a null values pointer to move the read pointer without reading any data.
 * Count may be negative in unbounded mode to seek backwards.
 * Note: This function is thread-safe */
int ringbuffer_consume(const RingBuffer rb, void * values, int count);

typedef bool (*RingBufferIterator)(int index, void * value, void * udata);
void ringbuffer_forEach(const RingBuffer rb, RingBufferIterator fn,
    void * udata, bool reverse);
