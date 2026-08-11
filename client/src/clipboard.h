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

#ifndef _H_LG_CLIENT_CLIPBOARD_
#define _H_LG_CLIENT_CLIPBOARD_

#include "interface/clipboard.h"

void lgClipboard_init(void);
void lgClipboard_free(void);
void lgClipboard_setLocalAvailable(bool available);

void lgClipboard_setFallback(const LG_ClipboardOps * ops, void * opaque);
/* Drop functions remove a dead endpoint without invoking it. */
void lgClipboard_dropFallback(void);
void lgClipboard_setTransport(const LG_ClipboardOps * ops, void * opaque);
void lgClipboard_dropTransport(void);

void lgClipboard_release(void);
void lgClipboard_notifyTypes(
    const LG_ClipboardData types[], size_t count);
void lgClipboard_data(LG_ClipboardRequest request,
    LG_ClipboardData type, const void * data, size_t size);
void lgClipboard_abort(LG_ClipboardRequest request);
bool lgClipboard_request(LG_ClipboardData type,
    LG_ClipboardReplyFn replyFn, void * opaque);

#endif
