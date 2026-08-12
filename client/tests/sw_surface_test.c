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
#include "test.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

struct LG_Transport
{
  atomic_bool entered;
  atomic_bool armed;
  atomic_bool release;
  atomic_uint cancelCount;
  atomic_uint on;
  atomic_uint off;
};

static bool setActive(LG_Transport * transport, bool active)
{
  atomic_fetch_add(active ? &transport->on : &transport->off, 1);
  atomic_store(&transport->entered, true);
  if (active)
  {
    /* Lose the first cancellation edge before arming the wait. */
    while (!atomic_load(&transport->cancelCount))
      usleep(1000);
    atomic_store(&transport->release, false);
    atomic_store(&transport->armed, true);
    while (!atomic_load(&transport->release))
      usleep(1000);
  }
  return !active || atomic_load(&transport->release);
}

static void cancelPending(LG_Transport * transport)
{
  atomic_fetch_add(&transport->cancelCount, 1);
  if (atomic_load(&transport->armed))
    atomic_store(&transport->release, true);
}

static bool cancelled(void * opaque)
{
  return *(const bool *)opaque;
}

static const LG_SwSurfaceOps ops =
{
  .setActive    = setActive,
  .cancelPending = cancelPending,
};

int main(void)
{
  LG_Transport transport = { 0 };
  const bool stop = true;
  CHECK(!lgSwSurface_setActive(
        &ops, &transport, true, cancelled, (void *)&stop));
  CHECK(atomic_load(&transport.entered));
  CHECK(atomic_load(&transport.cancelCount) >= 2);
  CHECK(atomic_load(&transport.on) == 1);
  CHECK(atomic_load(&transport.off) == 1);

  atomic_store(&transport.entered, false);
  atomic_store(&transport.release, false);
  atomic_store(&transport.cancelCount, 0);
  CHECK(lgSwSurface_setActive(
        &ops, &transport, false, cancelled, (void *)&stop));
  CHECK(atomic_load(&transport.entered));
  CHECK(atomic_load(&transport.cancelCount) == 0);
  CHECK(atomic_load(&transport.off) == 2);
  return EXIT_SUCCESS;
}
