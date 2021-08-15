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

#include "common/countedbuffer.h"
#include <stdlib.h>
#include <stdatomic.h>

struct CountedBuffer * countedBufferNew(size_t size)
{
  struct CountedBuffer * buffer = malloc(sizeof(*buffer) + size);
  if (!buffer)
    return NULL;

  atomic_init(&buffer->refs, 1);
  buffer->size = size;
  return buffer;
}

void countedBufferAddRef(struct CountedBuffer * buffer)
{
  atomic_fetch_add(&buffer->refs, 1);
}

void countedBufferRelease(struct CountedBuffer ** buffer)
{
  if (atomic_fetch_sub(&(*buffer)->refs, 1) == 1)
  {
    free(*buffer);
    *buffer = NULL;
  }
}
