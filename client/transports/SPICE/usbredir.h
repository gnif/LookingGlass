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

#ifndef _H_LG_CLIENT_USBREDIR_
#define _H_LG_CLIENT_USBREDIR_

#include "interface/usbredir.h"

#include <purespice.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct LG_USBRedir LG_USBRedir;

/* Availability transitions are delivered on the PureSpice process thread. */
typedef void (*LG_USBRedirStatusFn)(void * opaque, bool available);

LG_USBRedir * lgUsbRedir_create(
    const LG_USBRedirDeviceOps * deviceOps, void * deviceOpaque,
    LG_USBRedirStatusFn status, void * statusOpaque);
/* The PureSpice session must be stopped before destroying the bridge. */
void lgUsbRedir_destroy(LG_USBRedir * usbredir);

/* This may be called from another thread. The request is applied by
 * lgUsbRedir_process on the PureSpice processing thread. */
void lgUsbRedir_setPlugged(LG_USBRedir * usbredir, bool plugged);
bool lgUsbRedir_available(const LG_USBRedir * usbredir);
/* True after a device disconnect has been sent and until the guest has
 * completed the detach. This must be queried on the processing thread. */
bool lgUsbRedir_disconnectPending(const LG_USBRedir * usbredir);

/* Apply pending device state and flush parser output. This must be called on
 * the PureSpice processing thread. */
bool lgUsbRedir_process(LG_USBRedir * usbredir);

void lgUsbRedir_state(PSUSBRedirChannel * channel,
    PSUSBRedirState state, void * opaque);
bool lgUsbRedir_data(PSUSBRedirChannel * channel,
    const uint8_t * data, size_t size, void * opaque);

#endif
