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

#include <stddef.h>
#include <stdint.h>

#define KVMFR_CLIPBOARD_VERSION       1U
#define KVMFR_CLIPBOARD_SLOT_COUNT    2U
#define KVMFR_CLIPBOARD_DATA_BYTES    (64U * 1024U)
#define KVMFR_CLIPBOARD_SIZE_UNKNOWN  UINT64_MAX

enum
{
  KVMFR_CLIPBOARD_FORMAT_NONE = 0,
  KVMFR_CLIPBOARD_FORMAT_TEXT = 1,
  KVMFR_CLIPBOARD_FORMAT_PNG  = 2,
  KVMFR_CLIPBOARD_FORMAT_BMP  = 3,
  KVMFR_CLIPBOARD_FORMAT_TIFF = 4,
  KVMFR_CLIPBOARD_FORMAT_JPEG = 5
};

typedef uint32_t KVMFRClipboardFormat;

enum
{
  KVMFR_CLIPBOARD_FORMAT_MASK_TEXT = 1U << 0,
  KVMFR_CLIPBOARD_FORMAT_MASK_PNG  = 1U << 1,
  KVMFR_CLIPBOARD_FORMAT_MASK_BMP  = 1U << 2,
  KVMFR_CLIPBOARD_FORMAT_MASK_TIFF = 1U << 3,
  KVMFR_CLIPBOARD_FORMAT_MASK_JPEG = 1U << 4,
  KVMFR_CLIPBOARD_FORMAT_MASK_ALL  =
    KVMFR_CLIPBOARD_FORMAT_MASK_TEXT |
    KVMFR_CLIPBOARD_FORMAT_MASK_PNG  |
    KVMFR_CLIPBOARD_FORMAT_MASK_BMP  |
    KVMFR_CLIPBOARD_FORMAT_MASK_TIFF |
    KVMFR_CLIPBOARD_FORMAT_MASK_JPEG,
};

typedef uint32_t KVMFRClipboardFormatFlags;

static inline int kvmfrClipboardFormatValid(KVMFRClipboardFormat format)
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
  KVMFR_CLIPBOARD_MESSAGE_CLAIM       = 1,
  KVMFR_CLIPBOARD_MESSAGE_RELEASE     = 2,
  KVMFR_CLIPBOARD_MESSAGE_KEEPALIVE   = 3,
  KVMFR_CLIPBOARD_MESSAGE_OFFER       = 4,
  KVMFR_CLIPBOARD_MESSAGE_CLEAR       = 5,
  KVMFR_CLIPBOARD_MESSAGE_REQUEST     = 6,
  KVMFR_CLIPBOARD_MESSAGE_DATA        = 7,
  KVMFR_CLIPBOARD_MESSAGE_COMMIT      = 8,
  KVMFR_CLIPBOARD_MESSAGE_CANCEL      = 9,
  KVMFR_CLIPBOARD_MESSAGE_ACK         = 10,
  KVMFR_CLIPBOARD_MESSAGE_GRANT       = 11
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
 * pool and the same layout.
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
  KVMFR_CLIPBOARD_STATUS_AVAILABLE = 1U << 0,
  KVMFR_CLIPBOARD_STATUS_HAS_OWNER = 1U << 1
};

typedef uint32_t KVMFRClipboardStatusFlags;

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
}
KVMFRClipboardStatus;

/* The data immediately following this header is length bytes long. */
typedef KVMFRClipboardMessage KVMFRClipboardSlotHeader;

enum
{
  KVMFR_CLIPBOARD_QUEUE_STATUS  = 1,
  KVMFR_CLIPBOARD_QUEUE_MESSAGE = 2,
  KVMFR_CLIPBOARD_QUEUE_GRANT   = 3,
  KVMFR_CLIPBOARD_QUEUE_DATA    = 4
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
static_assert(sizeof(KVMFRClipboardStatus) == 32,
  "KVMFR clipboard status layout changed");
static_assert(sizeof(KVMFRClipboardSlotHeader) == 64,
  "KVMFR clipboard slot header layout changed");
static_assert(sizeof(KVMFRClipboardMessage) <= 64,
  "KVMFR clipboard message must fit one LGMP control message");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(KVMFRClipboardMessage) == 64,
  "KVMFR clipboard control message layout changed");
_Static_assert(sizeof(KVMFRClipboardStatus) == 32,
  "KVMFR clipboard status layout changed");
_Static_assert(sizeof(KVMFRClipboardSlotHeader) == 64,
  "KVMFR clipboard slot header layout changed");
_Static_assert(sizeof(KVMFRClipboardMessage) <= 64,
  "KVMFR clipboard message must fit one LGMP control message");
#endif

#endif
