/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "sw_surface.h"

#include "common/debug.h"
#include "common/thread.h"
#include "common/time.h"

#include <stdatomic.h>

struct SwSurfaceCall
{
  const LG_SwSurfaceOps * ops;
  LG_Transport          * transport;
  bool                    active;
  bool                    result;
  atomic_bool             done;
};

static int swSurfaceCallThread(void * opaque)
{
  struct SwSurfaceCall * call = opaque;
  call->result = call->ops->setActive(call->transport, call->active);
  atomic_store_explicit(&call->done, true, memory_order_release);
  return 0;
}

bool lgSwSurface_setActive(const LG_SwSurfaceOps * ops,
    LG_Transport * transport, bool active,
    LG_TransportCancelledFn cancelled, void * opaque)
{
  if (!ops || !ops->setActive || !ops->cancelPending || !transport)
    return false;

  struct SwSurfaceCall call =
  {
    .ops       = ops,
    .transport = transport,
    .active    = active,
  };
  atomic_init(&call.done, false);

  LGThread * thread;
  if (!lgCreateThread("surfaceState", swSurfaceCallThread, &call, &thread))
    return false;

  bool cancelRequested = false;
  while (!atomic_load_explicit(&call.done, memory_order_acquire))
  {
    if (active && cancelled && cancelled(opaque))
    {
      cancelRequested = true;
      ops->cancelPending(transport);
    }
    nsleep(1000000U);
  }

  if (active && cancelled && cancelled(opaque))
    cancelRequested = true;

  if (!lgJoinThread(thread, NULL))
  {
    DEBUG_ERROR("Failed to join software surface state worker");
    return false;
  }
  if (active && cancelRequested)
  {
    /* The backend may complete successfully at the same instant cancellation
     * is observed. Leave it inactive before reporting the cancelled result. */
    if (!ops->setActive(transport, false))
      DEBUG_ERROR("Failed to deactivate a cancelled software surface");
    return false;
  }
  return call.result;
}
