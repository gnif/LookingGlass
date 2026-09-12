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

#ifndef _H_I_EVENT_SOURCE_
#define _H_I_EVENT_SOURCE_

#include <stdbool.h>

typedef bool (*LG_DSEventWake)(void * opaque);
typedef bool (*LG_DSEventSubscribe)(void * opaque,
    LG_DSEventWake callback, void * callbackOpaque);
typedef void (*LG_DSEventUnsubscribe)(void * opaque);
typedef void (*LG_DSEventDispatch)(void * opaque);

typedef struct LG_DSEventSource
{
  void                  * opaque;
  LG_DSEventSubscribe     subscribe;
  LG_DSEventUnsubscribe   unsubscribe;
  LG_DSEventDispatch      dispatch;
}
LG_DSEventSource;

/* Notifications are coalesced until dispatch. The subscriber wake callback
 * must be nonblocking, must not call back into the source, and returns true
 * when the wake was delivered. Failed wakes may be retried by a later
 * notification. Unsubscribe is synchronous with any wake already in
 * progress. */

static inline bool lgEventSourceValid(const LG_DSEventSource * source)
{
  return source && source->subscribe && source->unsubscribe &&
    source->dispatch;
}

#endif
