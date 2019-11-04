/*
Looking Glass - KVM FrameRelay (KVMFR) Client
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

#include "types.h"
#include <sys/types.h>

/**
 * Copy from memory to a PortholeMap
 *
 * @param src The source buffer
 * @param dst The destination map
 * @param len The data length to copy
 * @param off The offset into the dst PortholeMap
 */
void porthole_copy_mem_to_map(void * src, PortholeMap * dst, size_t len, off_t off);

/**
 * Copy from a PortholeMap to memory
 *
 * @param src The source buffer
 * @param dst The destination buffer
 * @param len The data length to copy
 * @param off The offset into the src PortholeMap
 */
void porthole_copy_map_to_mem(PortholeMap * src, void * dst, size_t len, off_t off);

/**
 * Copy from a PortholeMap to a PortholeMap
 *
 * @param src     The source buffer
 * @param dst     The destination buffer
 * @param len     The data length to copy
 * @param src_off The offset into the src PortholeMap
 * @param dst_off The offset into the dst PortholeMap
 */
void porthole_copy_map_to_map(PortholeMap * src, PortholeMap * dst, size_t len, off_t src_off, off_t dst_off);