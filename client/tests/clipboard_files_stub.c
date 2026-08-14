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

#include "core/clipboard_files.h"

unsigned lgClipboardFilesStubRemoteOfferCount;
unsigned lgClipboardFilesStubRemoteClearCount;
uint64_t lgClipboardFilesStubRemoteDataset;
bool lgClipboardFilesStubRemoteReady;
bool lgClipboardFilesStubRemoteOfferResult = true;

bool lgClipboardFiles_init(void) { return true; }
void lgClipboardFiles_free(void) {}
bool lgClipboardFiles_setLocal(const char * mime,
    const void * data, size_t size)
{
  (void)mime; (void)data; (void)size; return false;
}
void lgClipboardFiles_clearLocal(void) {}
bool lgClipboardFiles_getRemote(const char * mime, char ** data, size_t * size)
{
  (void)mime; (void)data; (void)size; return false;
}
uint64_t lgClipboardFiles_remotePresentationAcquire(void) { return 0; }
bool lgClipboardFiles_getRemotePresentation(uint64_t presentation,
    const char * mime, char ** data, size_t * size)
{
  (void)presentation; (void)mime; (void)data; (void)size; return false;
}
void lgClipboardFiles_remotePresentationDelivered(uint64_t presentation)
{
  (void)presentation;
}
void lgClipboardFiles_remotePresentationRelease(uint64_t presentation)
{
  (void)presentation;
}
bool lgClipboardFiles_remoteReady(uint64_t dataset)
{
  return lgClipboardFilesStubRemoteReady &&
    dataset == lgClipboardFilesStubRemoteDataset;
}
bool lgClipboardFiles_remoteOffer(uint64_t dataset)
{
  ++lgClipboardFilesStubRemoteOfferCount;
  lgClipboardFilesStubRemoteDataset = dataset;
  return lgClipboardFilesStubRemoteOfferResult;
}
void lgClipboardFiles_remoteClear(void)
{
  ++lgClipboardFilesStubRemoteClearCount;
  lgClipboardFilesStubRemoteDataset = 0;
}
void lgClipboardFiles_providerUnavailable(void) {}
void lgClipboardFiles_remoteAcquired(uint64_t dataset,
    uint64_t acquisition, LG_ClipboardFileError error)
{
  (void)dataset; (void)acquisition; (void)error;
}
LG_ClipboardResult lgClipboardFiles_remoteDataBegin(
    const LG_ClipboardFileRequest * request, uint64_t sizeHint)
{
  (void)request; (void)sizeHint; return LG_CLIPBOARD_RESULT_FAILED;
}
LG_ClipboardResult lgClipboardFiles_remoteDataChunk(
    const LG_ClipboardFileRequest * request, uint64_t offset,
    const void * data, size_t size)
{
  (void)request; (void)offset; (void)data; (void)size;
  return LG_CLIPBOARD_RESULT_FAILED;
}
LG_ClipboardResult lgClipboardFiles_remoteDataEnd(
    const LG_ClipboardFileRequest * request, uint64_t size)
{
  (void)request; (void)size; return LG_CLIPBOARD_RESULT_FAILED;
}
void lgClipboardFiles_remoteCancel(uint64_t dataset,
    uint64_t request, LG_ClipboardFileError reason)
{
  (void)dataset; (void)request; (void)reason;
}
void lgClipboardFiles_localAcquire(uint64_t dataset, uint64_t acquisition)
{
  (void)dataset; (void)acquisition;
}
void lgClipboardFiles_localRelease(uint64_t dataset, uint64_t acquisition)
{
  (void)dataset; (void)acquisition;
}
void lgClipboardFiles_localRequest(const LG_ClipboardFileRequest * request)
{
  (void)request;
}
void lgClipboardFiles_localCancel(uint64_t dataset,
    uint64_t request, LG_ClipboardFileError reason)
{
  (void)dataset; (void)request; (void)reason;
}
void lgClipboardFiles_localReady(uint64_t request) { (void)request; }
