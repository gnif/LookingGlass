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

bool lgClipboard_init(void);
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
void lgClipboard_notifyFiles(uint64_t dataset);
LG_ClipboardResult lgClipboard_dataBegin(LG_ClipboardRequest request,
    LG_ClipboardData type, uint64_t sizeHint);
LG_ClipboardResult lgClipboard_dataChunk(LG_ClipboardRequest request,
    uint64_t offset, const void * data, size_t size);
LG_ClipboardResult lgClipboard_dataEnd(LG_ClipboardRequest request,
    uint64_t finalSize);
void lgClipboard_data(LG_ClipboardRequest request,
    LG_ClipboardData type, const void * data, size_t size);
void lgClipboard_abort(LG_ClipboardRequest request);
/* request is published before provider dispatch. Its storage must remain
 * valid until this function returns and may be observed by stream callbacks
 * before then. */
bool lgClipboard_requestStream(LG_ClipboardData type,
    const LG_ClipboardStreamOps * stream, void * opaque,
    LG_ClipboardRequest * request);
bool lgClipboard_requestReady(LG_ClipboardRequest request);
bool lgClipboard_request(LG_ClipboardData type,
    LG_ClipboardReplyFn replyFn, void * opaque);

/* File-dataset transport entry points used by clipboard_files.c. */
bool lgClipboard_fileAcquire(uint64_t dataset, uint64_t acquisition);
bool lgClipboard_fileAcquired(uint64_t dataset, uint64_t acquisition,
    LG_ClipboardFileError error);
bool lgClipboard_fileRelease(uint64_t dataset, uint64_t acquisition);
bool lgClipboard_fileRequest(const LG_ClipboardFileRequest * request);
LG_ClipboardResult lgClipboard_fileDataBegin(
    const LG_ClipboardFileRequest * request, uint64_t sizeHint);
LG_ClipboardResult lgClipboard_fileDataChunk(
    const LG_ClipboardFileRequest * request, uint64_t responseOffset,
    const void * data, size_t size);
LG_ClipboardResult lgClipboard_fileDataEnd(
    const LG_ClipboardFileRequest * request, uint64_t finalSize);
bool lgClipboard_fileCancel(uint64_t dataset, uint64_t request,
    LG_ClipboardFileError reason);
void lgClipboard_fileRemoteReady(uint64_t dataset);
void lgClipboard_fileRemoteFailed(uint64_t dataset);

#endif
