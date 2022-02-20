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

#ifndef _H_LL_
#define _H_LL_

#include <stdbool.h>
#include <stdlib.h>

#include "common/locking.h"

struct ll_item
{
  void           * data;
  struct ll_item * prev, * next;
};

struct ll
{
  struct ll_item * head;
  struct ll_item * tail;
  unsigned int count;
  LG_Lock lock;
};

struct ll *  ll_new(void);
void         ll_free     (struct ll * list);
void         ll_push     (struct ll * list, void * data);
bool         ll_shift    (struct ll * list, void ** data);
bool         ll_peek_head(struct ll * list, void ** data);
bool         ll_peek_tail(struct ll * list, void ** data);

#define ll_lock(ll) LG_LOCK((ll)->lock)
#define ll_unlock(ll) LG_UNLOCK((ll)->lock)

#define ll_forEachNL(ll, item, v) \
  for(struct ll_item * item = (ll)->head, \
      * _ = (item) ? (item)->next : NULL; (item); (item) = NULL) \
    for((v) = (__typeof__(v))((item)->data); (item); (item) = _, \
        _   = (item) ? (item)->next : NULL, \
        (v) = (item) ? (__typeof__(v))((item)->data) : NULL)

static inline unsigned int ll_count(struct ll * list)
{
  return list->count;
}

static inline void ll_removeNL(struct ll * list, struct ll_item * item)
{
  --list->count;

  if (list->head == item)
    list->head = item->next;

  if (list->tail == item)
    list->tail = item->prev;

  if (item->prev)
    item->prev->next = item->next;

  if (item->next)
    item->next->prev = item->prev;
}

static inline bool ll_removeData(struct ll * list, void * match)
{
  void * data;
  ll_lock(list);
  ll_forEachNL(list, item, data)
    if (data == match)
    {
      ll_removeNL(list, item);
      ll_unlock(list);
      free(item);
      return true;
    }
  ll_unlock(list);

  return false;
}

#endif
