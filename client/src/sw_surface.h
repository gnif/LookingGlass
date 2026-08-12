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

#ifndef _H_LG_CLIENT_SW_SURFACE_
#define _H_LG_CLIENT_SW_SURFACE_

#include "interface/transport.h"

/* Runs setActive on a worker and joins it before returning. Cancellation is
 * observed only while activating; deactivation is allowed to complete unless
 * an owner invokes cancelPending from another thread. */
bool lgSwSurface_setActive(const LG_SwSurfaceOps * ops,
    LG_Transport * transport, bool active,
    LG_TransportCancelledFn cancelled, void * opaque);

#endif
