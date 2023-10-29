/**
 * Looking Glass
 * Copyright © 2017-2023 The Looking Glass Authors
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

#include <stdbool.h>
#include <windows.h>

/**
 * These functions are to assist in tracking and relasing COM objects
 */

/**
 * Initialize the com object tracking
 */
bool comRef_init(unsigned globals, unsigned locals);

/**
 * Release globals and deinitialize the com object tracking
 */
void comRef_free(void);

/**
 * Create a new global COM reference
 */
IUnknown ** comRef_newGlobal(IUnknown *** dst);

/**
 * Create a new locally scoped COM reference
 */
IUnknown ** comRef_newLocal(IUnknown *** dst);

/**
 * Define and create a new locally scoped COM reference
 */
#define comRef_defineLocal(type, name) \
  type ** name; \
  comRef_newLocal(&name);

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

/**
 * Create a new local scope
 */
void comRef_scopePush(void);

/**
 * Exit from a local scope and release all locals
 */
void comRef_scopePop (void);

/**
 * Macros to prevent needing to typecast calls to these methods
 */
#ifndef COMREF_INTERNAL
  #define comRef_newGlobal(dst) comRef_newGlobal((IUnknown ***)(dst))
  #define comRef_newLocal(dst)  comRef_newLocal((IUnknown ***)(dst))
  #define comRef_release(ref)   comRef_release((IUnknown **)(ref))
#endif

/**
 * Convert a local to a global
 */
#define comRef_toGlobal(dst, src) \
{ \
  IUnknown ** global = comRef_newGlobal(&(dst)); \
  DEBUG_ASSERT(global && "comRef_newGlobal failed\n"); \
  *global = (IUnknown*)*(src); \
  *(src)  = NULL; \
}
