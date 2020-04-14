/*
KVMGFX Client - A KVM Client for VGA Passthrough
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#pragma once

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct stFrameBuffer FrameBuffer;

typedef bool (*FrameBufferReadFn)(void * opaque, const void * src, size_t size);

/**
 * The size of the FrameBuffer struct
 */
extern const size_t FrameBufferStructSize;

/**
 * Wait for the framebuffer to fill to the specified size
 */
void framebuffer_wait(const FrameBuffer * frame, size_t size);

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
