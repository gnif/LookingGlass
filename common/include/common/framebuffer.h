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

#ifndef _H_LG_COMMON_FRAMEBUFFER_
#define _H_LG_COMMON_FRAMEBUFFER_

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#define FB_CHUNK_SIZE           1048576 // 1MB
#define FB_SPIN_LIMIT           10000   // 10ms
#define FB_WP_TYPE              atomic_uint_least32_t
#define FB_WP_SIZE              sizeof(FB_WP_TYPE)

typedef struct stFrameBuffer
{
  FB_WP_TYPE wp;
  uint8_t    data[0];
} FrameBuffer;

typedef bool (*FrameBufferReadFn)(void * opaque, const void * src, size_t size);

/**
 * Wait for the framebuffer to fill to the specified size
 */
bool framebuffer_wait(const FrameBuffer * frame, size_t size);

/**
 * Read data from the KVMFRFrame into the dst buffer
 */
bool framebuffer_read(const FrameBuffer * frame, void * dst, size_t dstpitch,
    size_t height, size_t width, size_t bpp, size_t pitch);

/**
 * Read data from the KVMFRFrame using a callback
 */
bool framebuffer_read_fn(const FrameBuffer * frame, size_t height, size_t width,
    size_t bpp, size_t pitch, FrameBufferReadFn fn, void * opaque);

/**
 * Prepare the framebuffer for writing
 */
void framebuffer_prepare(FrameBuffer * frame);

/**
 * Write data from the src buffer into the KVMFRFrame
 */
bool framebuffer_write(FrameBuffer * frame, const void * src, size_t size);

/**
 * Gets the underlying data buffer of the framebuffer.
 * For custom read routines only.
 */
const uint8_t * framebuffer_get_buffer(const FrameBuffer * frame);

/**
 * Gets the underlying data buffer of the framebuffer.
 * For custom write routines only.
 */
uint8_t * framebuffer_get_data(FrameBuffer * frame);

/**
 * Sets the write pointer of the framebuffer.
 * For custom write routines only.
 */
void framebuffer_set_write_ptr(FrameBuffer * frame, size_t size);

#endif
