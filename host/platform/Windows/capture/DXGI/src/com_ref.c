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

#define COMREF_INTERNAL
#include "com_ref.h"

#include "common/debug.h"
#include "common/vector.h"

typedef struct
{
  int          scope;
  IUnknown *   value;
  IUnknown *** ref;
}
COMRef;

static bool   comInit          = false;
static int    comScope         = -1;
static Vector comObjectsLocal  = {0};
static Vector comObjectsGlobal = {0};

bool comRef_init(unsigned globals, unsigned locals)
{
  if (comInit)
    return true;

  if (!vector_create(&comObjectsGlobal, sizeof(COMRef), globals))
    return false;

  if (!vector_create(&comObjectsLocal, sizeof(COMRef), locals))
  {
    vector_destroy(&comObjectsGlobal);
    return false;
  }

  comInit = true;
  return true;
}

void comRef_free(void)
{
  if (!comInit)
    return;

  COMRef * ref;

  if (comScope > -1)
  {
    DEBUG_WARN("There is %d unmatched `comRef_scopePush` calls", comScope+1);
    vector_forEachRef(ref, &comObjectsLocal)
      if (ref->value)
        IUnknown_Release(ref->value);
  }

  vector_forEachRef(ref, &comObjectsGlobal)
  {
    if (ref->ref)
      *ref->ref = NULL;

    if (ref->value)
      IUnknown_Release(ref->value);
  }

  comScope = -1;
  vector_destroy(&comObjectsLocal);
  vector_destroy(&comObjectsGlobal);
  comInit = false;
}

static IUnknown ** comRef_new(Vector * vector, IUnknown *** dst)
{
  DEBUG_ASSERT(comInit && "comRef has not been initialized");

  // we must not allow the vector to grow as if the realloc moves to a new
  // address it will invalidate any external pointers to members in it
  DEBUG_ASSERT(vector_size(vector) < vector_capacity(vector) &&
    "comRef vector too small!");

  COMRef * ref = (COMRef *)vector_push(vector, NULL);
  if (!ref)
  {
    DEBUG_ERROR("Failed to allocate ram for com object");
    return NULL;
  }

  ref->scope = comScope;
  ref->ref   = dst;
  ref->value = NULL;

  if (dst)
    *dst = &ref->value;

  return &ref->value;
}

IUnknown ** comRef_newGlobal(IUnknown *** dst)
{
  return comRef_new(&comObjectsGlobal, dst);
}

IUnknown ** comRef_newLocal(IUnknown *** dst)
{
  IUnknown ** ret = comRef_new(&comObjectsLocal, NULL);
  *dst = ret;
  return ret;
}

void comRef_scopePush(void)
{
  DEBUG_ASSERT(comInit && "comRef has not been initialized");
  ++comScope;
}

void comRef_scopePop(void)
{
  DEBUG_ASSERT(comInit && "comRef has not been initialized");
  DEBUG_ASSERT(comScope >= 0);

  COMRef * ref;
  while(vector_size(&comObjectsLocal) > 0)
  {
    ref = (COMRef *)vector_ptrTo(&comObjectsLocal,
      vector_size(&comObjectsLocal) - 1);

    if (ref->scope < comScope)
      break;

    if (ref->value)
      IUnknown_Release(ref->value);

    vector_pop(&comObjectsLocal);
  }

  --comScope;
}
