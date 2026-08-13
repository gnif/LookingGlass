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

#ifndef _H_LG_CLIENT_TRANSPORT_LGMP_CLIPBOARD_
#define _H_LG_CLIENT_TRANSPORT_LGMP_CLIPBOARD_

#include "interface/clipboard.h"

#include <lgmp/client.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct LGMPClipboard LGMPClipboard;

bool lgmpClipboard_create(PLGMPClient client, LGMPClipboard ** result);
void lgmpClipboard_destroy(LGMPClipboard ** clipboard);

bool lgmpClipboard_connect(LGMPClipboard * clipboard, uint32_t clientID);
void lgmpClipboard_disconnect(LGMPClipboard * clipboard);

const LG_ClipboardOps * lgmpClipboard_getOps(void);

#endif
