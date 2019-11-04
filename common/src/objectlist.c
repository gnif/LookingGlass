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

#include "common/objectlist.h"

#include <stdlib.h>
#include <string.h>

struct ObjectList
{
  ObjectFreeFn    free_fn;
  unsigned int    size;
  unsigned int    count;
  void         ** list;
};

ObjectList objectlist_new(ObjectFreeFn free_fn)
{
  ObjectList ol = malloc(sizeof(struct ObjectList));

  ol->free_fn      = free_fn;
  ol->size         = 32;
  ol->count        = 0;
  ol->list         = malloc(sizeof(void *) * ol->size);

  return ol;
}

void objectlist_free(ObjectList * ol)
{
  if ((*ol)->free_fn)
    for(unsigned int i = 0; i < (*ol)->count; ++i)
      (*ol)->free_fn((*ol)->list[i]);

  free((*ol)->list);
  free((*ol));
  *ol = NULL;
}

int objectlist_push(ObjectList ol, void * object)
{
  if (ol->count == ol->size)
  {
    ol->size += 32;
    ol->list  = realloc(ol->list, sizeof(void *) * ol->size);
  }

  unsigned int index = ol->count;
  ol->list[ol->count++] = object;
  return index;
}

void * objectlist_pop(ObjectList ol)
{
  if (ol->count == 0)
    return NULL;

  return ol->list[--ol->count];
}

bool objectlist_remove(ObjectList ol, unsigned int index)
{
  if (index >= ol->count)
    return false;

  if (ol->free_fn)
    ol->free_fn(ol->list[index]);

  void ** newlist = malloc(sizeof(void *) * ol->size);

  --ol->count;
  memcpy(&newlist[0], &ol->list[0], index * sizeof(void *));
  memcpy(&newlist[index], &ol->list[index + 1], (ol->count - index) * sizeof(void *));

  free(ol->list);
  ol->list = newlist;
  return true;
}

unsigned int objectlist_count(ObjectList ol)
{
  return ol->count;
}

void * objectlist_at(ObjectList ol, unsigned int index)
{
  if (index >= ol->count)
    return NULL;

  return ol->list[index];
}

void objectlist_free_item(void *object)
{
  free(object);
}