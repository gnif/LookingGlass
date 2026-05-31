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

#ifndef _H_COMREF_
#define _H_COMREF_

#include <stdbool.h>
#include <windows.h>
#include <malloc.h>

#include "common/util.h"
#include "common/locking.h"

/**
 * These functions are to assist in tracking and releasing COM objects
 */

typedef struct ComScope ComScope;

struct ComScope
{
  bool          threadSafe;
  LG_Lock       lock;

  unsigned      size;
  unsigned      used;
  struct
  {
    IUnknown   * ref;
    const char * where;
  }
  *refs;

  void (*free)(void * ptr);
};

void comRef_initScope(unsigned size, ComScope ** instance,
  void *(allocFn)(size_t size), void (freeFn)(void * ptr), bool threadSafe);

void comRef_freeScope(ComScope ** instance);

IUnknown ** comRef_new(ComScope * scope, IUnknown *** dst, const char * where);

#define comRef_initGlobalScope(size, scope) \
  comRef_initScope((size), &(scope), malloc, free, true)

#define comRef_freeGlobalScope(scope) \
  comRef_freeScope(&(scope))

#define comRef_scopePush(size) \
  ComScope * _comRef_localScope = alloca(sizeof(*_comRef_localScope) + \
    sizeof(*(_comRef_localScope->refs)) * size); \
  comRef_initScope(size, &_comRef_localScope, NULL, NULL, false);

#define comRef_scopePop() \
  comRef_freeScope(&_comRef_localScope)

#define comRef_defineLocal(type, name) \
  type ** name = NULL; \
  comRef_new(_comRef_localScope, (IUnknown ***)&(name), \
    STR(name));

#define _comRef_toGlobal(globalScope, dst, src) \
{ \
  IUnknown ** global = comRef_new((globalScope), (IUnknown ***)&(dst), \
    STR(dst)); \
  *global = (IUnknown *)*(src); \
  *(src)  = NULL; \
}

/**
 * Release a COM reference immediately
 * This is just a helper, the ref is still tracked if used again
 */
inline static ULONG comRef_release(IUnknown ** ref)
{
  if (!ref)
    return 0;

  ULONG count = 0;
  if (*ref)
    count = IUnknown_Release(*ref);
  *ref = NULL;
  return count;
}

#define comRef_release(ref) comRef_release((IUnknown **)(ref))

#endif
