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

#ifndef _H_LG_CLIENT_CLIPBOARD_INTERFACE_
#define _H_LG_CLIENT_CLIPBOARD_INTERFACE_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum LG_ClipboardData
{
  LG_CLIPBOARD_DATA_TEXT = 0,
  LG_CLIPBOARD_DATA_PNG,
  LG_CLIPBOARD_DATA_BMP,
  LG_CLIPBOARD_DATA_TIFF,
  LG_CLIPBOARD_DATA_JPEG,
  LG_CLIPBOARD_DATA_FILES,

  LG_CLIPBOARD_DATA_NONE
}
LG_ClipboardData;

typedef void (*LG_ClipboardReplyFn)(void * opaque,
    LG_ClipboardData type, const uint8_t * data, uint32_t size);

typedef uint64_t LG_ClipboardRequest;

#define LG_CLIPBOARD_REQUEST_INVALID UINT64_C(0)
#define LG_CLIPBOARD_SIZE_UNKNOWN    UINT64_MAX

typedef enum LG_ClipboardResult
{
  /* The operation is terminal and consumed no data. */
  LG_CLIPBOARD_RESULT_FAILED = 0,
  /* The operation, including the complete chunk, was accepted. */
  LG_CLIPBOARD_RESULT_ACCEPTED = 1,
  /* Nothing was consumed. Retry only after the matching ready callback. */
  LG_CLIPBOARD_RESULT_BLOCKED = 2,
}
LG_ClipboardResult;

typedef enum LG_ClipboardCancelReason
{
  LG_CLIPBOARD_CANCEL_ABORTED = 0,
  LG_CLIPBOARD_CANCEL_REPLACED = 1,
  LG_CLIPBOARD_CANCEL_UNAVAILABLE = 2,
  LG_CLIPBOARD_CANCEL_INVALID = 3,
}
LG_ClipboardCancelReason;

typedef enum LG_ClipboardFileOperation
{
  LG_CLIPBOARD_FILE_LIST = 1,
  LG_CLIPBOARD_FILE_READ = 2,
}
LG_ClipboardFileOperation;

typedef enum LG_ClipboardFileError
{
  LG_CLIPBOARD_FILE_ERROR_NONE = 0,
  LG_CLIPBOARD_FILE_ERROR_NOT_FOUND,
  LG_CLIPBOARD_FILE_ERROR_ACCESS,
  LG_CLIPBOARD_FILE_ERROR_NOT_DIRECTORY,
  LG_CLIPBOARD_FILE_ERROR_IS_DIRECTORY,
  LG_CLIPBOARD_FILE_ERROR_IO,
  LG_CLIPBOARD_FILE_ERROR_INVALID,
  LG_CLIPBOARD_FILE_ERROR_NO_MEMORY,
  LG_CLIPBOARD_FILE_ERROR_NO_SPACE,
  LG_CLIPBOARD_FILE_ERROR_DISCONNECTED,
  LG_CLIPBOARD_FILE_ERROR_CANCELLED,
  LG_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED,
  LG_CLIPBOARD_FILE_ERROR_STALE,
}
LG_ClipboardFileError;

typedef struct LG_ClipboardFileRequest
{
  uint64_t dataset;
  uint64_t request;
  uint64_t node;
  uint64_t offset;
  uint32_t length;
  LG_ClipboardFileOperation operation;
}
LG_ClipboardFileRequest;

/* A consumer of a provider-to-client stream. Callbacks are serialized and
 * buffers are borrowed only for the duration of chunk(). A callback which
 * returns BLOCKED consumes nothing; the consumer must subsequently call
 * its request-ready entry point before the same operation can be retried. */
typedef struct LG_ClipboardStreamOps
{
  LG_ClipboardResult (*begin)(void * opaque, LG_ClipboardData type,
      uint64_t sizeHint);
  LG_ClipboardResult (*chunk)(void * opaque, uint64_t offset,
      const void * data, size_t size);
  LG_ClipboardResult (*end)(void * opaque, uint64_t finalSize);
  void (*cancel)(void * opaque, LG_ClipboardCancelReason reason);
}
LG_ClipboardStreamOps;

typedef struct LG_ClipboardStatus
{
  bool     available;
  uint32_t generation;
}
LG_ClipboardStatus;

typedef void (*LG_ClipboardStatusFn)(void * opaque,
    const LG_ClipboardStatus * status);

typedef struct LG_ClipboardEventOps
{
  /* The type list and data are borrowed for the duration of the callback.
   * Event callbacks are always invoked after releasing backend locks. */
  void (*notice)(void * opaque, const LG_ClipboardData types[], size_t count);
  void (*data)(void * opaque, LG_ClipboardRequest request,
      LG_ClipboardData type, const void * data, size_t size);
  LG_ClipboardResult (*dataBegin)(void * opaque,
      LG_ClipboardRequest request, LG_ClipboardData type,
      uint64_t sizeHint);
  LG_ClipboardResult (*dataChunk)(void * opaque,
      LG_ClipboardRequest request, uint64_t offset,
      const void * data, size_t size);
  LG_ClipboardResult (*dataEnd)(void * opaque,
      LG_ClipboardRequest request, uint64_t finalSize);
  void (*dataCancel)(void * opaque, LG_ClipboardRequest request,
      LG_ClipboardCancelReason reason);
  /* Signals that the provider can accept a retry of the operation which most
   * recently returned BLOCKED for its request. */
  void (*dataReady)(void * opaque, LG_ClipboardRequest request);
  /* Cancels a request previously delivered through request(). */
  void (*requestCancel)(void * opaque, LG_ClipboardRequest request,
      LG_ClipboardCancelReason reason);
  void (*release)(void * opaque);
  bool (*request)(void * opaque, LG_ClipboardRequest request,
      LG_ClipboardData type);

  /* File datasets use independent, multiplexed request streams. Dataset and
   * node identifiers are opaque outside the publisher. */
  void (*fileOffer)(void * opaque, uint64_t dataset);
  void (*fileAcquire)(void * opaque, uint64_t dataset,
      uint64_t acquisition);
  void (*fileAcquired)(void * opaque, uint64_t dataset,
      uint64_t acquisition, LG_ClipboardFileError error);
  void (*fileRelease)(void * opaque, uint64_t dataset,
      uint64_t acquisition);
  void (*fileRequest)(void * opaque,
      const LG_ClipboardFileRequest * request);
  LG_ClipboardResult (*fileDataBegin)(void * opaque,
      const LG_ClipboardFileRequest * request, uint64_t sizeHint);
  LG_ClipboardResult (*fileDataChunk)(void * opaque,
      const LG_ClipboardFileRequest * request, uint64_t responseOffset,
      const void * data, size_t size);
  LG_ClipboardResult (*fileDataEnd)(void * opaque,
      const LG_ClipboardFileRequest * request, uint64_t finalSize);
  void (*fileDataReady)(void * opaque, uint64_t request);
  void (*fileCancel)(void * opaque, uint64_t dataset,
      uint64_t request, LG_ClipboardFileError reason);
}
LG_ClipboardEventOps;

typedef struct LG_ClipboardOps
{
  const char * name;

  /* Operations must fail safely if the remote endpoint disappears. */

  /* Registration must synchronously report the current status after releasing
   * any backend locks. Status callbacks must be serialized and generations
   * must advance whenever availability changes. Passing NULL unregisters the
   * listener and synchronously quiesces its callbacks. Status callbacks must
   * not be delivered from another operation or event callback. */
  void (*setStatusListener)(void * opaque, LG_ClipboardStatusFn callback,
      void * callbackOpaque);

  /* Attach begins serialized event delivery and may synchronously replay the
   * current remote notice. Detach synchronously quiesces event callbacks.
   * Other operations must not deliver event callbacks synchronously. */
  bool (*attach)(void * opaque, const LG_ClipboardEventOps * events,
      void * eventOpaque);
  void (*detach)(void * opaque);

  bool (*release)(void * opaque);
  /* All outbound arrays and data are borrowed only until the call returns. */
  bool (*notifyTypes)(void * opaque, const LG_ClipboardData types[],
      size_t count);
  /* Publishes an immutable local file dataset. The nonzero dataset ID is
   * process-unique and is used as the wire clipboard generation. */
  bool (*offerFiles)(void * opaque, uint64_t dataset);
  /* Data is a complete response to a remote request. NONE with no payload
   * reports that the request could not be completed. This operation is the
   * legacy whole-buffer alternative to the stream operation group below. */
  bool (*data)(void * opaque, LG_ClipboardRequest request,
      LG_ClipboardData type, const void * data, size_t size);
  /* Streaming providers implement this complete group instead of data().
   * BEGIN may use LG_CLIPBOARD_SIZE_UNKNOWN. CHUNK is all-or-nothing and its
   * offset must be the next byte in the stream. END's size is authoritative.
   * A BLOCKED operation consumes nothing and is retried only after dataReady
   * is delivered through the attached event operations. Ready callbacks must
   * be serialized with other events, delivered without backend locks held,
   * and never delivered synchronously from one of these operations. */
  LG_ClipboardResult (*dataBegin)(void * opaque,
      LG_ClipboardRequest request, LG_ClipboardData type,
      uint64_t sizeHint);
  LG_ClipboardResult (*dataChunk)(void * opaque,
      LG_ClipboardRequest request, uint64_t offset,
      const void * data, size_t size);
  LG_ClipboardResult (*dataEnd)(void * opaque,
      LG_ClipboardRequest request, uint64_t finalSize);
  bool (*dataCancel)(void * opaque, LG_ClipboardRequest request,
      LG_ClipboardCancelReason reason);
  /* Signals that the client can accept a retry of a provider-to-client stream
   * operation which returned BLOCKED. */
  bool (*dataReady)(void * opaque, LG_ClipboardRequest request);
  /* A successful request produces exactly one matching data event unless the
   * provider is detached, becomes unavailable, or publishes a newer notice
   * or release first. */
  bool (*request)(void * opaque, LG_ClipboardRequest request,
      LG_ClipboardData type);

  bool (*fileAcquire)(void * opaque, uint64_t dataset,
      uint64_t acquisition);
  bool (*fileAcquired)(void * opaque, uint64_t dataset,
      uint64_t acquisition, LG_ClipboardFileError error);
  bool (*fileRelease)(void * opaque, uint64_t dataset,
      uint64_t acquisition);
  bool (*fileRequest)(void * opaque,
      const LG_ClipboardFileRequest * request);
  LG_ClipboardResult (*fileDataBegin)(void * opaque,
      const LG_ClipboardFileRequest * request, uint64_t sizeHint);
  LG_ClipboardResult (*fileDataChunk)(void * opaque,
      const LG_ClipboardFileRequest * request, uint64_t responseOffset,
      const void * data, size_t size);
  LG_ClipboardResult (*fileDataEnd)(void * opaque,
      const LG_ClipboardFileRequest * request, uint64_t finalSize);
  bool (*fileCancel)(void * opaque, uint64_t dataset,
      uint64_t request, LG_ClipboardFileError reason);
}
LG_ClipboardOps;

#endif
