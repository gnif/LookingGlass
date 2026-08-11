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

#ifndef _H_LG_CLIENT_TRANSPORT_FALLBACK_
#define _H_LG_CLIENT_TRANSPORT_FALLBACK_

#include "interface/transport.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct LG_TransportFallback LG_TransportFallback;

typedef struct LG_TransportFallbackEventOps
{
  /* The session is valid only for the duration of this callback. */
  void (*connected)(void * opaque, const LG_TransportSession * session);
  void (*disconnected)(void * opaque);
  void (*uuidMismatch)(void * opaque, const uint8_t primary[16],
      const uint8_t fallback[16]);
}
LG_TransportFallbackEventOps;

bool lgTransportFallback_start(const char * transportName,
    const LG_SwSurfaceEventOps * surfaceEvents, void * surfaceOpaque,
    const LG_TransportFallbackEventOps * eventOps, void * eventOpaque,
    LG_TransportFallback ** result);
void lgTransportFallback_stop(LG_TransportFallback ** fallback);

bool lgTransportFallback_ready(const LG_TransportFallback * fallback);
/* The requested state is retained across reconnects. */
void lgTransportFallback_requestVideoActive(
    LG_TransportFallback * fallback, bool active);

void lgTransportFallback_setPrimaryUUID(
    LG_TransportFallback * fallback, const uint8_t uuid[16]);
void lgTransportFallback_clearPrimaryUUID(
    LG_TransportFallback * fallback);

#endif
