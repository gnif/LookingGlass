/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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

#include "com_ref.h"

#include "common/debug.h"

void comRef_initScope(unsigned size, ComScope ** instance,
  void *(allocFn)(size_t size), void (freeFn)(void * ptr), bool threadSafe)
{
  ComScope * scope = *instance;

  const size_t ttlSize = sizeof(*scope) + sizeof(*(scope->refs)) * size;
  if (allocFn)
    scope = allocFn(ttlSize);
  DEBUG_ASSERT(scope && "No memory for scope");

  memset(scope, 0, ttlSize);
  scope->threadSafe = threadSafe;
  if (threadSafe)
    LG_LOCK_INIT(scope->lock);

  scope->size       = size;
  scope->refs       = (typeof(scope->refs))(scope+1);
  scope->free       = freeFn;

  *instance = scope;
}

void comRef_freeScope(ComScope ** instance)
{
  if (!*instance)
    return;

  ComScope * scope = *instance;
  for(unsigned i = 0; i < scope->used; ++i)
  {
    typeof(scope->refs) ref = &scope->refs[i];
    if (ref->ref)
    {
      ULONG count = IUnknown_Release(ref->ref);

#ifdef DEBUG_COMREF
      if (count > 0 && scope->free)
        DEBUG_INFO("comRef %s release is %lu", ref->where, count);
#else
      (void)count;
#endif

      ref->ref = NULL;
    }
  }

  if (scope->threadSafe)
    LG_LOCK_FREE(scope->lock);

  if (scope->free)
    scope->free(scope);

  *instance = NULL;
}

IUnknown ** comRef_new(ComScope * scope, IUnknown *** dst, const char * where)
{
  /* check if the value it points to is already in our memory range and if it
   * does, then reuse it */
  if ((uintptr_t)*dst >= (uintptr_t)(scope->refs) &&
      (uintptr_t)*dst <  (uintptr_t)(scope->refs + scope->used))
  {
    // if it already holds a value, release it before we overwrite
    if (**dst)
    {
      IUnknown_Release(**dst);
      **dst = NULL;
    }

    // return the existing member
    return *dst;
  }

  /* If you hit this, you need to enlarge the scope size, or check if you have a
   * resource leak. */
  DEBUG_ASSERT(scope->used < scope->size && "ComRef Scope Full");

  if (scope->threadSafe)
    LG_LOCK(scope->lock);

  typeof(scope->refs[0]) * ref = &scope->refs[scope->used];
  ref->where = where;
  *dst = &ref->ref;

  ++scope->used;

  if (scope->threadSafe)
    LG_UNLOCK(scope->lock);

  return *dst;
}
