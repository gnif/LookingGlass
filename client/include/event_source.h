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

#ifndef _H_LG_EVENT_SOURCE_
#define _H_LG_EVENT_SOURCE_

#include "interface/event_source.h"

#include "common/locking.h"

typedef struct LGEventSourceState
{
  LG_Lock            lock;
  LG_DSEventWake     callback;
  void             * callbackOpaque;
  LG_DSEventDispatch dispatch;
  void             * dispatchOpaque;
  bool               pending;
}
LGEventSourceState;

bool lgEventSourceInit(LGEventSourceState * state,
    LG_DSEventDispatch dispatch, void * dispatchOpaque);
void lgEventSourceFree(LGEventSourceState * state);
LG_DSEventSource lgEventSourceGet(LGEventSourceState * state);
void lgEventSourceNotify(LGEventSourceState * state);

#endif
