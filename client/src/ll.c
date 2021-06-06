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

#include "ll.h"

#include "common/locking.h"
#include <stdlib.h>
#include <assert.h>

struct ll_item
{
  void           * data;
  struct ll_item * next;
};

struct ll
{
  struct ll_item * head;
  struct ll_item * tail;
  struct ll_item * pos;
  unsigned int count;
  LG_Lock lock;
};

struct ll * ll_new(void)
{
  struct ll * list = malloc(sizeof(struct ll));
  list->head  = NULL;
  list->tail  = NULL;
  list->pos   = NULL;
  list->count = 0;
  LG_LOCK_INIT(list->lock);
  return list;
}

void ll_free(struct ll * list)
{
  // never free a list with items in it!
  assert(!list->head);

  LG_LOCK_FREE(list->lock);
  free(list);
}

void ll_push(struct ll * list, void * data)
{
  struct ll_item * item = malloc(sizeof(struct ll_item));
  item->data = data;
  item->next = NULL;

  LG_LOCK(list->lock);
  ++list->count;

  if (!list->head)
  {
    list->head = item;
    list->tail = item;
    LG_UNLOCK(list->lock);
    return;
  }

  list->tail->next = item;
  list->tail       = item;
  LG_UNLOCK(list->lock);
}

bool ll_shift(struct ll * list, void ** data)
{
  LG_LOCK(list->lock);
  if (!list->head)
  {
    LG_UNLOCK(list->lock);
    return false;
  }

  --list->count;
  struct ll_item * item = list->head;
  list->head = item->next;
  list->pos  = NULL;
  if (list->tail == item)
    list->tail = NULL;

  LG_UNLOCK(list->lock);

  if (data)
    *data = item->data;

  free(item);
  return true;
}

bool ll_peek_head(struct ll * list, void ** data)
{
  LG_LOCK(list->lock);
  if (!list->head)
  {
    LG_UNLOCK(list->lock);
    return false;
  }

  *data = list->head->data;
  LG_UNLOCK(list->lock);

  return true;
}

bool ll_peek_tail(struct ll * list, void ** data)
{
  LG_LOCK(list->lock);
  if (!list->tail)
  {
    LG_UNLOCK(list->lock);
    return false;
  }

  *data = list->tail->data;
  LG_UNLOCK(list->lock);

  return true;
}

unsigned int ll_count(struct ll * list)
{
  return list->count;
}

void ll_reset (struct ll * list)
{
  LG_LOCK(list->lock);
  list->pos = NULL;
  LG_UNLOCK(list->lock);
}


bool ll_walk(struct ll * list, void ** data)
{
  LG_LOCK(list->lock);

  if (!list->pos)
  {
    if (!list->head)
    {
      LG_UNLOCK(list->lock);
      return false;
    }

    list->pos = list->head;
  }
  else
  {
    if (!list->pos->next)
    {
      LG_UNLOCK(list->lock);
      return false;
    }
    list->pos = list->pos->next;
  }

  *data = list->pos->data;
  LG_UNLOCK(list->lock);

  return true;
}
