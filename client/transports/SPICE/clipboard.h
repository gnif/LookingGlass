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

#ifndef _H_LG_CLIENT_TRANSPORT_SPICE_CLIPBOARD_
#define _H_LG_CLIENT_TRANSPORT_SPICE_CLIPBOARD_

#include "interface/clipboard.h"

#include <purespice.h>

typedef struct SpiceClipboard SpiceClipboard;

bool spiceClipboard_init(SpiceClipboard ** clipboard);
void spiceClipboard_free(SpiceClipboard ** clipboard);

const LG_ClipboardOps * spiceClipboard_getOps(void);
void spiceClipboard_setAvailable(SpiceClipboard * clipboard, bool available);

/* PureSpice's clipboard callbacks do not carry an opaque pointer. The active
 * callback target must be set before processing the corresponding session. */
void spiceClipboard_setCallbackTarget(SpiceClipboard * clipboard);
void spiceClipboard_notice(PSDataType type);
void spiceClipboard_data(PSDataType type, uint8_t * buffer, uint32_t size);
void spiceClipboard_release(void);
void spiceClipboard_request(PSDataType type);
void spiceClipboard_status(bool available);

#endif
