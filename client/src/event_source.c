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

#include "event_source.h"

#include <string.h>

static bool subscribe(void * opaque, LG_DSEventWake callback,
    void * callbackOpaque)
{
  LGEventSourceState * state = (LGEventSourceState *)opaque;
  if (!callback)
    return false;

  LG_LOCK(state->lock);
  if (state->callback)
  {
    LG_UNLOCK(state->lock);
    return false;
  }

  state->callback       = callback;
  state->callbackOpaque = callbackOpaque;
  if (state->pending && !state->callback(state->callbackOpaque))
  {
    state->callback       = NULL;
    state->callbackOpaque = NULL;
    LG_UNLOCK(state->lock);
    return false;
  }
  LG_UNLOCK(state->lock);
  return true;
}

static void unsubscribe(void * opaque)
{
  LGEventSourceState * state = (LGEventSourceState *)opaque;

  LG_LOCK(state->lock);
  state->callback       = NULL;
  state->callbackOpaque = NULL;
  LG_UNLOCK(state->lock);
}

static void dispatch(void * opaque)
{
  LGEventSourceState * state = (LGEventSourceState *)opaque;

  LG_LOCK(state->lock);
  state->pending = false;
  LG_UNLOCK(state->lock);
  state->dispatch(state->dispatchOpaque);
}

bool lgEventSourceInit(LGEventSourceState * state,
    LG_DSEventDispatch dispatchCallback, void * dispatchOpaque)
{
  if (!state || !dispatchCallback)
    return false;

  memset(state, 0, sizeof(*state));
  LG_LOCK_INIT(state->lock);
  state->dispatch       = dispatchCallback;
  state->dispatchOpaque = dispatchOpaque;
  return true;
}

void lgEventSourceFree(LGEventSourceState * state)
{
  unsubscribe(state);
  LG_LOCK_FREE(state->lock);
}

LG_DSEventSource lgEventSourceGet(LGEventSourceState * state)
{
  if (!state || !state->dispatch)
    return (LG_DSEventSource) { 0 };

  return (LG_DSEventSource)
  {
    .opaque      = state,
    .subscribe   = subscribe,
    .unsubscribe = unsubscribe,
    .dispatch    = dispatch,
  };
}

void lgEventSourceNotify(LGEventSourceState * state)
{
  LG_LOCK(state->lock);
  if (!state->pending)
  {
    state->pending = true;
    if (state->callback && !state->callback(state->callbackOpaque))
      state->pending = false;
  }
  LG_UNLOCK(state->lock);
}
