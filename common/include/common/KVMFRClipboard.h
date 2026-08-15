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

#ifndef _H_LG_COMMON_KVMFR_CLIPBOARD_
#define _H_LG_COMMON_KVMFR_CLIPBOARD_

#pragma once

#include "KVMFRStream.h"

#include <stddef.h>
#include <stdint.h>

#define KVMFR_CLIPBOARD_VERSION                4U
#define KVMFR_CLIPBOARD_STREAM_VERSION         1U

/* Physical stream geometry. Keep these independent from the maximum logical
 * request and representation sizes below: one response may span several
 * stream slots. The generic names are also used by the Helper/IDD clipboard
 * ring, which intentionally has the same geometry. */
#define KVMFR_CLIPBOARD_STREAM_SLOT_COUNT      4U
#define KVMFR_CLIPBOARD_STREAM_SLOT_BYTES      (256U * 1024U)
#define KVMFR_CLIPBOARD_STREAM_WINDOW_BYTES    \
  (KVMFR_CLIPBOARD_STREAM_SLOT_COUNT *         \
   KVMFR_CLIPBOARD_STREAM_SLOT_BYTES)
#define KVMFR_CLIPBOARD_SLOT_COUNT             \
  KVMFR_CLIPBOARD_STREAM_SLOT_COUNT
#define KVMFR_CLIPBOARD_DATA_BYTES             \
  KVMFR_CLIPBOARD_STREAM_SLOT_BYTES
/* Keep text and image chunks within the core X11 request limit. File data can
 * use the full slot without passing through XChangeProperty. */
#define KVMFR_CLIPBOARD_REPRESENTATION_BYTES   (64U * 1024U)
#define KVMFR_CLIPBOARD_SIZE_UNKNOWN           UINT64_MAX
/* Logical request size. A READ response may require multiple physical
 * stream records and must not be constrained to a single slot. */
#define KVMFR_CLIPBOARD_FILE_READ_BYTES        (1024U * 1024U)
#define KVMFR_CLIPBOARD_FILE_ROOT_NODE         UINT64_C(0)
#define KVMFR_CLIPBOARD_FILE_MAX_ACQUISITIONS  8U
#define KVMFR_CLIPBOARD_FILE_MAX_REQUESTS      32U

/* Transfer IDs are selected by the side which sends REQUEST. Keeping the
 * namespaces disjoint makes a simultaneous transfer in each direction
 * unambiguous when CANCEL and DATA cross in flight. */
#define KVMFR_CLIPBOARD_TRANSFER_HELPER (UINT64_C(1) << 63)

static inline int kvmfrClipboardTransferFromHelper(uint64_t transfer)
{
  return (transfer & KVMFR_CLIPBOARD_TRANSFER_HELPER) != 0;
}

static inline int kvmfrClipboardTransferFromClient(uint64_t transfer)
{
  return transfer != 0 && !kvmfrClipboardTransferFromHelper(transfer);
}

enum
{
  KVMFR_CLIPBOARD_FORMAT_NONE  = 0,
  KVMFR_CLIPBOARD_FORMAT_TEXT  = 1,
  KVMFR_CLIPBOARD_FORMAT_PNG   = 2,
  KVMFR_CLIPBOARD_FORMAT_BMP   = 3,
  KVMFR_CLIPBOARD_FORMAT_TIFF  = 4,
  KVMFR_CLIPBOARD_FORMAT_JPEG  = 5,
  KVMFR_CLIPBOARD_FORMAT_FILES = 6
};

typedef uint32_t KVMFRClipboardFormat;

enum
{
  KVMFR_CLIPBOARD_FORMAT_MASK_TEXT  = 1U << 0,
  KVMFR_CLIPBOARD_FORMAT_MASK_PNG   = 1U << 1,
  KVMFR_CLIPBOARD_FORMAT_MASK_BMP   = 1U << 2,
  KVMFR_CLIPBOARD_FORMAT_MASK_TIFF  = 1U << 3,
  KVMFR_CLIPBOARD_FORMAT_MASK_JPEG  = 1U << 4,
  KVMFR_CLIPBOARD_FORMAT_MASK_FILES = 1U << 5,
  KVMFR_CLIPBOARD_FORMAT_MASK_ALL   =
    KVMFR_CLIPBOARD_FORMAT_MASK_TEXT |
    KVMFR_CLIPBOARD_FORMAT_MASK_PNG  |
    KVMFR_CLIPBOARD_FORMAT_MASK_BMP  |
    KVMFR_CLIPBOARD_FORMAT_MASK_TIFF |
    KVMFR_CLIPBOARD_FORMAT_MASK_JPEG |
    KVMFR_CLIPBOARD_FORMAT_MASK_FILES,
};

typedef uint32_t KVMFRClipboardFormatFlags;

static inline int kvmfrClipboardFormatValid(KVMFRClipboardFormat format)
{
  return format >= KVMFR_CLIPBOARD_FORMAT_TEXT &&
    format <= KVMFR_CLIPBOARD_FORMAT_FILES;
}

static inline int kvmfrClipboardRepresentationFormatValid(
  KVMFRClipboardFormat format)
{
  return format >= KVMFR_CLIPBOARD_FORMAT_TEXT &&
    format <= KVMFR_CLIPBOARD_FORMAT_JPEG;
}

static inline KVMFRClipboardFormatFlags kvmfrClipboardFormatFlag(
  KVMFRClipboardFormat format)
{
  return kvmfrClipboardFormatValid(format) ?
    (KVMFRClipboardFormatFlags)(1U << (format - 1U)) : 0U;
}

enum
{
  KVMFR_CLIPBOARD_MESSAGE_CLAIM         = 1,
  KVMFR_CLIPBOARD_MESSAGE_RELEASE       = 2,
  KVMFR_CLIPBOARD_MESSAGE_KEEPALIVE     = 3,
  KVMFR_CLIPBOARD_MESSAGE_OFFER         = 4,
  KVMFR_CLIPBOARD_MESSAGE_CLEAR         = 5,
  KVMFR_CLIPBOARD_MESSAGE_REQUEST       = 6,
  KVMFR_CLIPBOARD_MESSAGE_DATA          = 7,
  KVMFR_CLIPBOARD_MESSAGE_CANCEL        = 8,
  KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE  = 9,
  KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED = 10,
  KVMFR_CLIPBOARD_MESSAGE_FILE_RELEASE  = 11,
  KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST  = 12,
  KVMFR_CLIPBOARD_MESSAGE_FILE_DATA     = 13,
  KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL   = 14
};

typedef uint32_t KVMFRClipboardMessageType;

enum
{
  KVMFR_CLIPBOARD_FLAG_BEGIN = 1U << 0,
  KVMFR_CLIPBOARD_FLAG_END   = 1U << 1
};

typedef uint32_t KVMFRClipboardFlags;

/*
 * Bidirectional control record. Client-to-host records must fit LGMP's
 * 64-byte client message area. Host-to-client records use a small payload
 * pool and the same layout. DATA and FILE_DATA records are carried only by
 * the duplex streams advertised in KVMFRClipboardStatus; all other records
 * use the clipboard queue. CLAIM flags are reserved and must be zero.
 */
typedef struct KVMFRClipboardMessage
{
  uint32_t                    version;
  KVMFRClipboardMessageType   type;
  uint32_t                    generation;
  uint32_t                    sequence;
  uint64_t                    clipboardGeneration;
  uint64_t                    transfer;
  uint64_t                    offset;
  uint64_t                    size;
  KVMFRClipboardFormat        format;
  uint32_t                    flags;
  uint32_t                    token;
  uint32_t                    length;
}
KVMFRClipboardMessage;

enum
{
  KVMFR_CLIPBOARD_FILE_OP_LIST = 1,
  KVMFR_CLIPBOARD_FILE_OP_READ = 2
};

typedef uint32_t KVMFRClipboardFileOperation;

enum
{
  KVMFR_CLIPBOARD_FILE_TYPE_REGULAR   = 1,
  KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY = 2
};

typedef uint32_t KVMFRClipboardFileType;

enum
{
  KVMFR_CLIPBOARD_FILE_ERROR_NONE          = 0,
  KVMFR_CLIPBOARD_FILE_ERROR_NOT_FOUND     = 1,
  KVMFR_CLIPBOARD_FILE_ERROR_ACCESS        = 2,
  KVMFR_CLIPBOARD_FILE_ERROR_NOT_DIRECTORY = 3,
  KVMFR_CLIPBOARD_FILE_ERROR_IS_DIRECTORY  = 4,
  KVMFR_CLIPBOARD_FILE_ERROR_IO            = 5,
  KVMFR_CLIPBOARD_FILE_ERROR_INVALID       = 6,
  KVMFR_CLIPBOARD_FILE_ERROR_NO_MEMORY     = 7,
  KVMFR_CLIPBOARD_FILE_ERROR_NO_SPACE      = 8,
  KVMFR_CLIPBOARD_FILE_ERROR_DISCONNECTED  = 9,
  KVMFR_CLIPBOARD_FILE_ERROR_CANCELLED     = 10,
  KVMFR_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED = 11,
  KVMFR_CLIPBOARD_FILE_ERROR_STALE         = 12
};

typedef uint32_t KVMFRClipboardFileError;

static inline int kvmfrClipboardFileErrorValid(
  KVMFRClipboardFileError error)
{
  return error <= KVMFR_CLIPBOARD_FILE_ERROR_STALE;
}

/*
 * A LIST response is a stream of these headers, each immediately followed by
 * nameLength bytes of an unescaped UTF-8 path component and then zero padding
 * to the next eight-byte boundary. Node zero is the dataset's synthetic root
 * and is never returned as an entry. Times are nanoseconds since the Unix
 * epoch. A zero creation time means the source cannot report it.
 */
typedef struct KVMFRClipboardFileEntry
{
  uint64_t node;
  uint64_t size;
  uint64_t createdNs;
  uint64_t modifiedNs;
  KVMFRClipboardFileType type;
  uint32_t nameLength;
}
KVMFRClipboardFileEntry;

#define KVMFR_CLIPBOARD_FILE_ENTRY_ALIGN 8U
#define KVMFR_CLIPBOARD_FILE_ENTRY_BYTES(nameLength) \
  ((uint64_t)(sizeof(KVMFRClipboardFileEntry) + (uint64_t)(nameLength) + \
    (KVMFR_CLIPBOARD_FILE_ENTRY_ALIGN - 1U)) & \
    ~(uint64_t)(KVMFR_CLIPBOARD_FILE_ENTRY_ALIGN - 1U))

/* File message field use:
 *
 * FILE_ACQUIRE / FILE_ACQUIRED / FILE_RELEASE:
 *   clipboardGeneration identifies the immutable dataset and transfer is the
 *   acquisition ID selected by the requester. format is FILES. ACQUIRED token
 *   is a KVMFRClipboardFileError; all other fields are zero.
 *
 * FILE_REQUEST:
 *   clipboardGeneration identifies the acquired dataset, transfer is a unique
 *   request ID, size is the opaque node ID, token is LIST or READ, and format
 *   is FILES. LIST requires offset and flags to be zero. READ uses offset as
 *   the file offset and flags as the requested byte count (at most
 *   KVMFR_CLIPBOARD_FILE_READ_BYTES). length is always zero because REQUEST
 *   has no attached payload.
 *
 * FILE_DATA:
 *   clipboardGeneration, transfer, format and token echo FILE_REQUEST. offset
 *   is relative to this response (not the source file), sequence starts at
 *   zero, and size follows DATA's BEGIN/END total-size rules. Chunks for
 *   different request IDs may be interleaved.
 *
 * FILE_CANCEL:
 *   clipboardGeneration and transfer identify an acquisition or request,
 *   format is FILES, and token is a non-zero KVMFRClipboardFileError.
 */

static inline int kvmfrClipboardFileOperationValid(
  KVMFRClipboardFileOperation operation)
{
  return operation == KVMFR_CLIPBOARD_FILE_OP_LIST ||
    operation == KVMFR_CLIPBOARD_FILE_OP_READ;
}

static inline int kvmfrClipboardFileTransferValid(uint64_t transfer)
{
  return transfer != 0;
}

static inline int kvmfrClipboardFileMessageType(
  KVMFRClipboardMessageType type)
{
  return type >= KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE &&
    type <= KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL;
}

static inline int kvmfrClipboardFileMessageValid(
  const KVMFRClipboardMessage * message)
{
  if (!message || message->version != KVMFR_CLIPBOARD_VERSION ||
      !message->clipboardGeneration ||
      !kvmfrClipboardFileTransferValid(message->transfer) ||
      message->format != KVMFR_CLIPBOARD_FORMAT_FILES ||
      message->length > KVMFR_CLIPBOARD_DATA_BYTES)
    return 0;

  switch (message->type)
  {
    case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE:
    case KVMFR_CLIPBOARD_MESSAGE_FILE_RELEASE:
      return !message->sequence && !message->offset && !message->size &&
        !message->flags && !message->token && !message->length;

    case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED:
      return !message->sequence && !message->offset && !message->size &&
        !message->flags &&
        kvmfrClipboardFileErrorValid(message->token) && !message->length;

    case KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST:
      if (message->sequence ||
          !kvmfrClipboardFileOperationValid(message->token) ||
          message->length ||
          (message->token == KVMFR_CLIPBOARD_FILE_OP_READ &&
            !message->size))
        return 0;
      if (message->token == KVMFR_CLIPBOARD_FILE_OP_LIST)
        return !message->offset && !message->flags;
      return message->flags &&
        message->flags <= KVMFR_CLIPBOARD_FILE_READ_BYTES &&
        message->offset <= UINT64_MAX - message->flags;

    case KVMFR_CLIPBOARD_MESSAGE_FILE_DATA:
    {
      if (!kvmfrClipboardFileOperationValid(message->token) ||
          (message->flags & ~(KVMFR_CLIPBOARD_FLAG_BEGIN |
            KVMFR_CLIPBOARD_FLAG_END)) ||
          (!message->length &&
            !(message->flags & KVMFR_CLIPBOARD_FLAG_END)) ||
          message->offset > UINT64_MAX - message->length)
        return 0;
      const uint64_t end = message->offset + message->length;
      if (message->flags & KVMFR_CLIPBOARD_FLAG_END)
        return message->size == end;
      if (!(message->flags & KVMFR_CLIPBOARD_FLAG_BEGIN))
        return message->size == KVMFR_CLIPBOARD_SIZE_UNKNOWN;
      return message->size == KVMFR_CLIPBOARD_SIZE_UNKNOWN ||
        message->size >= end;
    }

    case KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL:
      return !message->sequence && !message->offset && !message->size &&
        !message->flags && message->token &&
        kvmfrClipboardFileErrorValid(message->token) && !message->length;

    default:
      return 0;
  }
}

/* Type-specific fields:
 * CLAIM: token is the endpoint generation from the latest status.
 * OFFER:  token is KVMFRClipboardFormatFlags.
 * REQUEST: format and transfer identify the requested representation.
 * DATA: size is an optional total hint on BEGIN and authoritative on END;
 *       offset/length describe this record's borrowed payload.
 * CANCEL: token is a KVMFRClipboardCancelReason-compatible reason. */

enum
{
  KVMFR_CLIPBOARD_STATUS_AVAILABLE = 1U << 0,
  KVMFR_CLIPBOARD_STATUS_HAS_OWNER = 1U << 1
};

typedef uint32_t KVMFRClipboardStatusFlags;

/* The duplex stream descriptors are always valid. Their slot size includes
 * KVMFRClipboardSlotHeader as well as slotBytes of payload. */
typedef struct KVMFRClipboardStatus
{
  uint32_t                     version;
  KVMFRClipboardStatusFlags    flags;
  uint32_t                     generation;
  uint32_t                     ownerClientID;
  uint32_t                     ownerGeneration;
  uint32_t                     lease;
  KVMFRClipboardFormatFlags    formats;
  uint32_t                     slotBytes;
  uint32_t                     streamVersion;
  uint32_t                     streamSlotCount;
  uint32_t                     reserved[6];
  KVMFRStreamDescriptor        hostToClient;
  KVMFRStreamDescriptor        clientToHost;
}
KVMFRClipboardStatus;

/* The data immediately following this header is length bytes long. */
typedef KVMFRClipboardMessage KVMFRClipboardSlotHeader;

#define KVMFR_CLIPBOARD_STREAM_RECORD_BYTES \
  (sizeof(KVMFRClipboardSlotHeader) + KVMFR_CLIPBOARD_STREAM_SLOT_BYTES)

enum
{
  KVMFR_CLIPBOARD_QUEUE_STATUS  = 1,
  KVMFR_CLIPBOARD_QUEUE_MESSAGE = 2
};

typedef uint32_t KVMFRClipboardQueueType;

#define KVMFR_CLIPBOARD_QUEUE_UDATA(type, serial) \
  ((((uint64_t)(type)) << 32) | (uint32_t)(serial))
#define KVMFR_CLIPBOARD_QUEUE_TYPE(udata) \
  ((KVMFRClipboardQueueType)((uint64_t)(udata) >> 32))
#define KVMFR_CLIPBOARD_QUEUE_SERIAL(udata) ((uint32_t)(udata))

#if defined(__cplusplus)
static_assert(sizeof(KVMFRClipboardMessage) == 64,
  "KVMFR clipboard control message layout changed");
static_assert(sizeof(KVMFRClipboardFileEntry) == 40,
  "KVMFR clipboard file entry layout changed");
static_assert(offsetof(KVMFRClipboardStatus, streamVersion) == 32,
  "KVMFR clipboard stream status layout changed");
static_assert(offsetof(KVMFRClipboardStatus, hostToClient) == 64,
  "KVMFR clipboard stream discovery layout changed");
static_assert(sizeof(KVMFRClipboardStatus) == 128,
  "KVMFR clipboard status layout changed");
static_assert(sizeof(KVMFRClipboardSlotHeader) == 64,
  "KVMFR clipboard slot header layout changed");
static_assert(sizeof(KVMFRClipboardMessage) <= 64,
  "KVMFR clipboard message must fit one LGMP control message");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(KVMFRClipboardMessage) == 64,
  "KVMFR clipboard control message layout changed");
_Static_assert(sizeof(KVMFRClipboardFileEntry) == 40,
  "KVMFR clipboard file entry layout changed");
_Static_assert(offsetof(KVMFRClipboardStatus, streamVersion) == 32,
  "KVMFR clipboard stream status layout changed");
_Static_assert(offsetof(KVMFRClipboardStatus, hostToClient) == 64,
  "KVMFR clipboard stream discovery layout changed");
_Static_assert(sizeof(KVMFRClipboardStatus) == 128,
  "KVMFR clipboard status layout changed");
_Static_assert(sizeof(KVMFRClipboardSlotHeader) == 64,
  "KVMFR clipboard slot header layout changed");
_Static_assert(sizeof(KVMFRClipboardMessage) <= 64,
  "KVMFR clipboard message must fit one LGMP control message");
#endif

#endif
