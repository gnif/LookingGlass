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

#include "common/stringlist.h"
#include "common/vector.h"
#include "common/debug.h"

#include <stdlib.h>

struct StringList
{
  bool   owns_strings;
  Vector vector;
};

StringList stringlist_new(bool owns_strings)
{
  StringList sl = malloc(sizeof(*sl));
  if (!sl)
  {
    DEBUG_ERROR("out of memory");
    return NULL;
  }

  sl->owns_strings = owns_strings;

  if (!vector_create(&sl->vector, sizeof(char *), 32))
  {
    free(sl);
    return NULL;
  }
  return sl;
}

void stringlist_free(StringList * sl)
{
  stringlist_clear(*sl);

  vector_destroy(&(*sl)->vector);
  free((*sl));
  *sl = NULL;
}

int stringlist_push(StringList sl, char * str)
{
  int index = vector_size(&sl->vector);
  vector_push(&sl->vector, &str);
  return index;
}

void stringlist_remove(StringList sl, unsigned int index)
{
  vector_remove(&sl->vector, index);
}

unsigned int stringlist_count(StringList sl)
{
  return vector_size(&sl->vector);
}

char * stringlist_at(StringList sl, unsigned int index)
{
  if (index >= vector_size(&sl->vector))
    return NULL;

  char * ptr;
  vector_at(&sl->vector, index, &ptr);
  return ptr;
}

void stringlist_clear(StringList sl)
{
  if (sl->owns_strings)
  {
    char * ptr;
    vector_forEach(ptr, &sl->vector)
      free(ptr);
  }

  vector_clear(&sl->vector);
}
