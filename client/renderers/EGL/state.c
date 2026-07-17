/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
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

#include "state.h"

#include <stdatomic.h>
#include <string.h>

_Thread_local EGL_StateCache g_eglState;
static _Thread_local unsigned int localGeneration;
static atomic_uint sharedGeneration = 1;

static void stateInvalidate(unsigned int generation)
{
  memset(&g_eglState, 0, sizeof(g_eglState));
  localGeneration = generation;
}

void egl_stateInvalidate(void)
{
  stateInvalidate(atomic_load_explicit(
      &sharedGeneration, memory_order_relaxed));
}

void egl_stateInvalidateShared(void)
{
  const unsigned int generation = atomic_fetch_add_explicit(
      &sharedGeneration, 1, memory_order_release) + 1;
  stateInvalidate(generation);
}

void egl_stateCheckShared(void)
{
  const unsigned int generation = atomic_load_explicit(
      &sharedGeneration, memory_order_acquire);
  if (localGeneration != generation)
    stateInvalidate(generation);
}
