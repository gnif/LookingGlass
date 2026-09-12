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
#include "test.h"

struct Trace
{
  unsigned int         notifications;
  unsigned int         wakeAttempts;
  unsigned int         wakeFailures;
  unsigned int         dispatches;
  bool                 notifyDuringDispatch;
  LGEventSourceState * state;
};

static bool notify(void * opaque)
{
  struct Trace * trace = (struct Trace *)opaque;
  ++trace->wakeAttempts;
  if (trace->wakeFailures)
  {
    --trace->wakeFailures;
    return false;
  }

  ++trace->notifications;
  return true;
}

static void dispatch(void * opaque)
{
  struct Trace * trace = (struct Trace *)opaque;
  ++trace->dispatches;
  if (trace->notifyDuringDispatch)
  {
    trace->notifyDuringDispatch = false;
    lgEventSourceNotify(trace->state);
  }
}

int main(void)
{
  struct Trace       trace = { 0 };
  LGEventSourceState state;
  LG_DSEventSource   source;

  CHECK(!lgEventSourceInit(NULL, dispatch, &trace));
  CHECK(!lgEventSourceInit(&state, NULL, &trace));
  source = lgEventSourceGet(NULL);
  CHECK(!lgEventSourceValid(&source));

  CHECK(lgEventSourceInit(&state, dispatch, &trace));
  trace.state = &state;
  source      = lgEventSourceGet(&state);
  CHECK(lgEventSourceValid(&source));

  lgEventSourceNotify(&state);
  CHECK(trace.notifications == 0);
  CHECK(trace.wakeAttempts == 0);
  CHECK(source.subscribe(source.opaque, notify, &trace));
  CHECK(trace.notifications == 1);
  CHECK(trace.wakeAttempts == 1);
  CHECK(!source.subscribe(source.opaque, notify, &trace));

  lgEventSourceNotify(&state);
  CHECK(trace.notifications == 1);
  CHECK(trace.wakeAttempts == 1);
  source.dispatch(source.opaque);
  CHECK(trace.dispatches == 1);

  lgEventSourceNotify(&state);
  lgEventSourceNotify(&state);
  CHECK(trace.notifications == 2);
  CHECK(trace.wakeAttempts == 2);
  source.dispatch(source.opaque);
  CHECK(trace.dispatches == 2);

  trace.wakeFailures = 1;
  lgEventSourceNotify(&state);
  CHECK(trace.notifications == 2);
  CHECK(trace.wakeAttempts == 3);
  lgEventSourceNotify(&state);
  CHECK(trace.notifications == 3);
  CHECK(trace.wakeAttempts == 4);

  trace.notifyDuringDispatch = true;
  source.dispatch(source.opaque);
  CHECK(trace.dispatches == 3);
  CHECK(trace.notifications == 4);
  CHECK(trace.wakeAttempts == 5);
  source.dispatch(source.opaque);
  CHECK(trace.dispatches == 4);

  source.unsubscribe(source.opaque);
  lgEventSourceNotify(&state);
  CHECK(trace.notifications == 4);
  CHECK(trace.wakeAttempts == 5);
  trace.wakeFailures = 1;
  CHECK(!source.subscribe(source.opaque, notify, &trace));
  CHECK(trace.notifications == 4);
  CHECK(trace.wakeAttempts == 6);
  CHECK(source.subscribe(source.opaque, notify, &trace));
  CHECK(trace.notifications == 5);
  CHECK(trace.wakeAttempts == 7);
  source.dispatch(source.opaque);
  CHECK(trace.dispatches == 5);

  source.unsubscribe(source.opaque);
  lgEventSourceFree(&state);
  return 0;
}
