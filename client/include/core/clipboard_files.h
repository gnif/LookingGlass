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

bool clipboardFiles_init(void);
void clipboardFiles_free(void);

/* Desktop backends install a local file selection after reading one of the
 * standard URI clipboard representations. The payload is copied. */
bool clipboardFiles_setLocal(const char * mime,
    const void * data, size_t size);
void clipboardFiles_clearLocal(void);

/* Returns an allocated desktop representation for the current guest offer. */
bool clipboardFiles_getRemote(const char * mime,
    char ** data, size_t * size);
uint64_t clipboardFiles_remotePresentationAcquire(void);
bool clipboardFiles_getRemotePresentation(uint64_t presentation,
    const char * mime, char ** data, size_t * size);
void clipboardFiles_remotePresentationDelivered(uint64_t presentation);
void clipboardFiles_remotePresentationRelease(uint64_t presentation);
bool clipboardFiles_remoteReady(uint64_t dataset);

/* Events delivered by the active clipboard transport. */
bool clipboardFiles_remoteOffer(uint64_t dataset);
void clipboardFiles_remoteClear(void);
void clipboardFiles_providerUnavailable(void);
void clipboardFiles_remoteAcquired(uint64_t dataset,
    uint64_t acquisition, LG_ClipboardFileError error);
LG_ClipboardResult clipboardFiles_remoteDataBegin(
    const LG_ClipboardFileRequest * request, uint64_t sizeHint);
LG_ClipboardResult clipboardFiles_remoteDataChunk(
    const LG_ClipboardFileRequest * request, uint64_t responseOffset,
    const void * data, size_t size);
LG_ClipboardResult clipboardFiles_remoteDataEnd(
    const LG_ClipboardFileRequest * request, uint64_t finalSize);
void clipboardFiles_remoteCancel(uint64_t dataset,
    uint64_t request, LG_ClipboardFileError reason);

void clipboardFiles_localAcquire(uint64_t dataset,
    uint64_t acquisition);
void clipboardFiles_localRelease(uint64_t dataset,
    uint64_t acquisition);
void clipboardFiles_localRequest(
    const LG_ClipboardFileRequest * request);
void clipboardFiles_localCancel(uint64_t dataset,
    uint64_t request, LG_ClipboardFileError reason);
void clipboardFiles_localReady(uint64_t request);

#ifdef ENABLE_TESTS
bool clipboardFiles_testInit(uint64_t nonce);
bool clipboardFiles_testFuseStopWake(void);
bool clipboardFiles_testUnsentRemoteOwnership(void);
size_t clipboardFiles_testRemoteDatasetCount(void);
void clipboardFiles_testExpireRemoteDeliveries(void);
bool clipboardFiles_testBeginRemoteLookup(uint64_t presentation);
void clipboardFiles_testEndRemoteLookup(uint64_t presentation);
bool clipboardFiles_testRemoteRead(uint64_t presentation,
    uint64_t node, uint64_t offset, uint32_t length);
void clipboardFiles_testForceLocalEof(void);
bool clipboardFiles_testLocalRequest(
    const LG_ClipboardFileRequest * request);
#endif

#endif
