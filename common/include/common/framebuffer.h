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

#ifndef _H_LG_COMMON_FRAMEBUFFER_
#define _H_LG_COMMON_FRAMEBUFFER_

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include <LGProtocol/KVMFR.h>

#define FB_CHUNK_SIZE           1048576 // 1MB
#define FB_SPIN_LIMIT           10000   // 10ms

typedef bool (*FrameBufferReadFn)(void * opaque, const void * src, size_t size);

/**
 * Wait for the framebuffer to fill to the specified size
 */
bool framebuffer_wait(const KVMFRFrameBuffer * frame, size_t size);

/**
 * Wait for the framebuffer to fill to the specified size and accumulate the
 * nanoseconds spent waiting for the producer in `waitTimeNs`.
 */
bool framebuffer_wait_timed(const KVMFRFrameBuffer * frame, size_t size,
    uint64_t * waitTimeNs);

/**
 * Read `size` bytes from the KVMFRFrame into the dst buffer
 */
bool framebuffer_read_linear(const KVMFRFrameBuffer * frame,
    void * restrict dst, size_t size);

/**
 * Read data from the KVMFRFrame into the dst buffer
 */
bool framebuffer_read(const KVMFRFrameBuffer * frame, void * dst,
    size_t dstpitch, size_t height, size_t width, size_t bpp, size_t pitch);

/**
 * Read data from the KVMFRFrame and accumulate the nanoseconds spent waiting
 * for the producer in `waitTimeNs`.
 */
bool framebuffer_read_timed(const KVMFRFrameBuffer * frame, void * dst,
    size_t dstpitch, size_t height, size_t width, size_t bpp, size_t pitch,
    uint64_t * waitTimeNs);

/**
 * Read data from the KVMFRFrame using a callback
 */
bool framebuffer_read_fn(const KVMFRFrameBuffer * frame, size_t height,
    size_t width, size_t bpp, size_t pitch, FrameBufferReadFn fn,
    void * opaque);

/**
 * Prepare the framebuffer for writing
 */
void framebuffer_prepare(KVMFRFrameBuffer * frame);

/**
 * Write data from the src buffer into the KVMFRFrame
 */
extern bool (*framebuffer_write)(KVMFRFrameBuffer * frame,
    const void * restrict src, size_t size);

/**
 * Gets the underlying data buffer of the framebuffer.
 * For custom read routines only.
 */
const uint8_t * framebuffer_get_buffer(const KVMFRFrameBuffer * frame);

/**
 * Gets the underlying data buffer of the framebuffer.
 * For custom write routines only.
 */
uint8_t * framebuffer_get_data(KVMFRFrameBuffer * frame);

/**
 * Sets the write pointer of the framebuffer.
 * For custom write routines only.
 */
void framebuffer_set_write_ptr(KVMFRFrameBuffer * frame, size_t size);

#endif
