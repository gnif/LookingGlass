/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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

#include <stdlib.h>

struct StringList
{
  bool         owns_strings;
  unsigned int size;
  unsigned int count;
  char ** list;
};

StringList stringlist_new(bool owns_strings)
{
  StringList sl = malloc(sizeof(struct StringList));

  sl->owns_strings = owns_strings;
  sl->size         = 32;
  sl->count        = 0;
  sl->list         = malloc(sizeof(char *) * sl->size);

  return sl;
}

void stringlist_free(StringList * sl)
{
  if ((*sl)->owns_strings)
    for(unsigned int i = 0; i < (*sl)->count; ++i)
      free((*sl)->list[i]);

  free((*sl)->list);
  free((*sl));
  *sl = NULL;
}

int stringlist_push (StringList sl, char * str)
{
  if (sl->count == sl->size)
  {
    sl->size += 32;
    sl->list  = realloc(sl->list, sizeof(char *) * sl->size);
  }

  unsigned int index = sl->count;
  sl->list[sl->count++] = str;
  return index;
}

unsigned int stringlist_count(StringList sl)
{
  return sl->count;
}

char * stringlist_at(StringList sl, unsigned int index)
{
  if (index >= sl->count)
    return NULL;

  return sl->list[index];
}