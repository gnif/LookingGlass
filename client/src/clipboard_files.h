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

#ifndef _H_LG_CLIENT_CLIPBOARD_FILES_
#define _H_LG_CLIENT_CLIPBOARD_FILES_

#include "interface/clipboard.h"

bool lgClipboardFiles_init(void);
void lgClipboardFiles_free(void);

/* Desktop backends install a local file selection after reading one of the
 * standard URI clipboard representations. The payload is copied. */
bool lgClipboardFiles_setLocal(const char * mime,
    const void * data, size_t size);
void lgClipboardFiles_clearLocal(void);

/* Returns an allocated desktop representation for the current guest offer. */
bool lgClipboardFiles_getRemote(const char * mime,
    char ** data, size_t * size);
uint64_t lgClipboardFiles_remotePresentationAcquire(void);
bool lgClipboardFiles_getRemotePresentation(uint64_t presentation,
    const char * mime, char ** data, size_t * size);
void lgClipboardFiles_remotePresentationDelivered(uint64_t presentation);
void lgClipboardFiles_remotePresentationRelease(uint64_t presentation);
bool lgClipboardFiles_remoteReady(uint64_t dataset);

/* Events delivered by the active clipboard transport. */
bool lgClipboardFiles_remoteOffer(uint64_t dataset);
void lgClipboardFiles_remoteClear(void);
void lgClipboardFiles_providerUnavailable(void);
void lgClipboardFiles_remoteAcquired(uint64_t dataset,
    uint64_t acquisition, LG_ClipboardFileError error);
LG_ClipboardResult lgClipboardFiles_remoteDataBegin(
    const LG_ClipboardFileRequest * request, uint64_t sizeHint);
LG_ClipboardResult lgClipboardFiles_remoteDataChunk(
    const LG_ClipboardFileRequest * request, uint64_t responseOffset,
    const void * data, size_t size);
LG_ClipboardResult lgClipboardFiles_remoteDataEnd(
    const LG_ClipboardFileRequest * request, uint64_t finalSize);
void lgClipboardFiles_remoteCancel(uint64_t dataset,
    uint64_t request, LG_ClipboardFileError reason);

void lgClipboardFiles_localAcquire(uint64_t dataset,
    uint64_t acquisition);
void lgClipboardFiles_localRelease(uint64_t dataset,
    uint64_t acquisition);
void lgClipboardFiles_localRequest(
    const LG_ClipboardFileRequest * request);
void lgClipboardFiles_localCancel(uint64_t dataset,
    uint64_t request, LG_ClipboardFileError reason);
void lgClipboardFiles_localReady(uint64_t request);

#ifdef ENABLE_TESTS
bool lgClipboardFiles_testInit(uint64_t nonce);
bool lgClipboardFiles_testFuseStopWake(void);
bool lgClipboardFiles_testUnsentRemoteOwnership(void);
size_t lgClipboardFiles_testRemoteDatasetCount(void);
void lgClipboardFiles_testExpireRemoteDeliveries(void);
bool lgClipboardFiles_testBeginRemoteLookup(uint64_t presentation);
void lgClipboardFiles_testEndRemoteLookup(uint64_t presentation);
bool lgClipboardFiles_testRemoteRead(uint64_t presentation,
    uint64_t node, uint64_t offset, uint32_t length);
void lgClipboardFiles_testForceLocalEof(void);
bool lgClipboardFiles_testLocalRequest(
    const LG_ClipboardFileRequest * request);
#endif

#endif
