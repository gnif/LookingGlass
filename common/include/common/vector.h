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

#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct Vector
{
  size_t itemSize;
  size_t size;
  size_t capacity;
  void * data;
}
Vector;

Vector * vector_create(size_t itemSize, size_t capacity);
void vector_free(Vector * vector);

bool vector_push(Vector * vector, void * item);
void vector_pop(Vector * vector);
size_t vector_size(Vector * vector);
void * vector_data(Vector * vector);
void vector_at(Vector * vector, size_t index, void * data);
void * vector_ptrTo(Vector * vector, size_t index);
void vector_clear(Vector * vector);

#define vector_forEach(name, vector) \
  for (char * vecIterCurrent = (vector)->data, \
            * vecIterEnd = vecIterCurrent + (vector)->size * (vector)->itemSize; \
       vecIterCurrent < vecIterEnd ? name = *(__typeof__(name) *)vecIterCurrent, true : false; \
       vecIterCurrent += (vector)->itemSize)

#define vector_forEachRef(name, vector) \
  for (char * vecIterCurrent = (vector)->data, \
            * vecIterEnd = vecIterCurrent + (vector)->size * (vector)->itemSize; \
       vecIterCurrent < vecIterEnd ? name = (void *)vecIterCurrent, true : false; \
       vecIterCurrent += (vector)->itemSize)
