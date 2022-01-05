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

#include "common/vector.h"
#include "common/debug.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

Vector * vector_alloc(size_t itemSize, size_t capacity)
{
  Vector * vector = malloc(sizeof(Vector));
  if (!vector)
    return NULL;

  if (!vector_create(vector, itemSize, capacity))
  {
    free(vector);
    return NULL;
  }
  return vector;
}

void vector_free(Vector * vector)
{
  if (!vector)
    return;
  vector_destroy(vector);
  free(vector);
}

bool vector_create(Vector * vector, size_t itemSize, size_t capacity)
{
  vector->itemSize = itemSize;
  vector->capacity = capacity;
  vector->size     = 0;
  vector->data     = NULL;

  if (capacity)
  {
    vector->data = malloc(itemSize * capacity);
    if (!vector->data)
      return false;
  }
  return true;
}

void vector_destroy(Vector * vector)
{
  free(vector->data);
  vector->capacity = 0;
  vector->itemSize = 0;
}

void * vector_push(Vector * vector, void * item)
{
  if (vector->size >= vector->capacity)
  {
    size_t newCapacity = vector->capacity < 4 ? 8 : vector->capacity * 2;
    void * new = realloc(vector->data, newCapacity * vector->itemSize);
    if (!new)
    {
      DEBUG_ERROR("Failed to allocate memory in vector: %" PRIuPTR " bytes", newCapacity * vector->itemSize);
      return NULL;
    }

    vector->capacity = newCapacity;
    vector->data = new;
  }

  void * ptr = (char *)vector->data + vector->size * vector->itemSize;
  if (item)
    memcpy(ptr, item, vector->itemSize);
  ++vector->size;
  return ptr;
}

void vector_pop(Vector * vector)
{
  DEBUG_ASSERT(vector->size > 0 && "Attempting to pop from empty vector!");
  --vector->size;
}

void vector_remove(Vector * vector, size_t index)
{
  DEBUG_ASSERT(index < vector->size && "Attempting to remove non-existent index!");
  memmove((char *)vector->data + index * vector->itemSize,
    (char *)vector->data + (index + 1) * vector->itemSize,
    (vector->size - index - 1) * vector->itemSize
  );
  --vector->size;
}

void vector_at(Vector * vector, size_t index, void * data)
{
  DEBUG_ASSERT(index < vector->size && "Out of bounds access");
  memcpy(data, (char *)vector->data + index * vector->itemSize, vector->itemSize);
}

void * vector_ptrTo(Vector * vector, size_t index)
{
  DEBUG_ASSERT(index < vector->size && "Out of bounds access");
  return (char *)vector->data + index * vector->itemSize;
}

void vector_clear(Vector * vector)
{
  vector->size = 0;
}
