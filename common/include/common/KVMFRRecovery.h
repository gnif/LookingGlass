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

#ifndef _H_LG_COMMON_KVMFR_RECOVERY_
#define _H_LG_COMMON_KVMFR_RECOVERY_

#pragma once

#include <stddef.h>
#include <stdint.h>

#define KVMFR_R_MAGIC   "KVMFRRCV"
#define KVMFR_R_VERSION 1U
#define KVMFR_R_READY   1U

#define KVMFR_R_REGION_SIZE  65536U
#define KVMFR_R_LINE_SIZE    64U
#define KVMFR_R_HEARTBEAT_MS 250U
#define KVMFR_R_REQ_SLOTS    16U

enum
{
  KVMFR_R_CAP_DISPLAY = 0x1U
};

enum
{
  KVMFR_R_REQ_WRITING = 0x1U,
  KVMFR_R_REQ_FIRST   = 0x2U
};

enum
{
  KVMFR_R_REQ_NONE     = 0,
  KVMFR_R_REQ_NORMAL   = 1,
  KVMFR_R_REQ_RECOVERY = 2
};

enum
{
  KVMFR_R_STATE_UNKNOWN   = 0,
  KVMFR_R_STATE_NORMAL    = 1,
  KVMFR_R_STATE_SWITCHING = 2,
  KVMFR_R_STATE_ACTIVE    = 3,
  KVMFR_R_STATE_FAILED    = 4
};

enum
{
  KVMFR_R_ERR_NONE                = 0,
  KVMFR_R_ERR_UNSUPPORTED         = 1,
  KVMFR_R_ERR_HELPER_UNAVAILABLE  = 2,
  KVMFR_R_ERR_TOPOLOGY_FAILED     = 3,
  KVMFR_R_ERR_NO_FALLBACK_DISPLAY = 4,
  KVMFR_R_ERR_BUSY                = 5,
  KVMFR_R_ERR_CAPACITY            = 6
};

/*
 * The recovery region is outside LGMP and remains stable across LGMP and
 * KVMFR protocol changes. Header, information, and status lines are
 * producer-owned. Request lines are client-owned except that the producer
 * atomically clears a completed slot's serial. Shared fields are plain
 * fixed-width values; synchronization is supplied externally rather than
 * embedded in the wire layout.
 */
typedef struct KVMFRRHeader
{
  char     magic[8];
  uint16_t abiVersion;
  uint16_t structSize;
  uint32_t capabilities;
  uint32_t lgmpVersion;
  uint32_t kvmfrVersion;
  uint64_t session;
  uint8_t  uuid[16];
  uint32_t heartbeat;
  uint32_t reserved[2];
  uint32_t ready;
}
KVMFRRHeader;

/*
 * Initialized by the IDD before publishing KVMFRRHeader::ready with
 * release ordering. Clients acquire ready before reading either producer
 * line. The heartbeat is an independently published 32-bit counter. The
 * version string is NUL-terminated.
 */
typedef struct KVMFRRInfo
{
  char    version[48];
  uint8_t reserved[16];
}
KVMFRRInfo;

/*
 * Written by clients. Ticket is an even monotonic counter used to allocate a
 * unique request serial. Zero is skipped when it wraps.
 */
typedef struct KVMFRRReqHead
{
  uint32_t ticket;
  uint8_t  reserved[60];
}
KVMFRRReqHead;

/*
 * Written by clients. A writer claims an empty slot by changing serial from
 * zero to its odd ticket, writes the payload, then release-publishes the even
 * ticket. The IDD ignores odd slots and atomically clears completed even
 * slots. An interrupted writer only consumes its own slot and cannot block or
 * corrupt another writer. Session rejects requests from an old producer
 * instance.
 */
typedef struct KVMFRRRequest
{
  uint32_t serial;
  uint32_t request;
  uint64_t session;
  uint8_t  reserved[48];
}
KVMFRRRequest;

/*
 * Written only by the IDD. Serial is an independent publication generation:
 * the IDD makes it odd before changing the payload, then publishes the next
 * nonzero even value with release ordering. Clients use it to take a coherent
 * snapshot. AckSerial identifies the request being acknowledged and does not
 * provide publication ordering because it remains unchanged as a request
 * moves through states.
 */
typedef struct KVMFRRStatus
{
  uint32_t ackSerial;
  uint32_t ackRequest;
  uint32_t state;
  uint32_t error;
  uint64_t session;
  uint32_t serial;
  uint8_t  reserved[36];
}
KVMFRRStatus;

typedef struct KVMFRR
{
  KVMFRRHeader  header;
  KVMFRRInfo    info;
  KVMFRRReqHead req;
  KVMFRRRequest requests[KVMFR_R_REQ_SLOTS];
  KVMFRRStatus  status;
}
KVMFRR;

#if defined(__cplusplus)
static_assert(KVMFR_R_REGION_SIZE % KVMFR_R_LINE_SIZE == 0,
  "KVMFR recovery region must contain whole cache lines");
static_assert(sizeof(KVMFRRHeader) == KVMFR_R_LINE_SIZE,
  "KVMFR recovery header must occupy one cache line");
static_assert(offsetof(KVMFRRHeader, lgmpVersion) == 16,
  "KVMFR recovery protocol version layout changed");
static_assert(offsetof(KVMFRRHeader, session) == 24,
  "KVMFR recovery session layout changed");
static_assert(offsetof(KVMFRRHeader, uuid) == 32,
  "KVMFR recovery UUID layout changed");
static_assert(offsetof(KVMFRRHeader, heartbeat) == 48,
  "KVMFR recovery heartbeat layout changed");
static_assert(offsetof(KVMFRRHeader, ready) == 60,
  "KVMFR recovery publication layout changed");
static_assert(sizeof(KVMFRRInfo) == KVMFR_R_LINE_SIZE,
  "KVMFR recovery producer information must occupy one cache line");
static_assert(sizeof(KVMFRRReqHead) == KVMFR_R_LINE_SIZE,
  "KVMFR recovery request header must occupy one cache line");
static_assert(sizeof(KVMFRRRequest) == KVMFR_R_LINE_SIZE,
  "KVMFR recovery request must occupy one cache line");
static_assert(offsetof(KVMFRRRequest, session) == 8,
  "KVMFR recovery request session layout changed");
static_assert(offsetof(KVMFRRRequest, request) == 4,
  "KVMFR recovery request publication layout changed");
static_assert(sizeof(KVMFRRStatus) == KVMFR_R_LINE_SIZE,
  "KVMFR recovery status must occupy one cache line");
static_assert(offsetof(KVMFRRStatus, session) == 16,
  "KVMFR recovery status session layout changed");
static_assert(offsetof(KVMFRRStatus, serial) == 24,
  "KVMFR recovery status publication layout changed");
static_assert(offsetof(KVMFRR, info) == KVMFR_R_LINE_SIZE,
  "KVMFR recovery producer information must be cache-line aligned");
static_assert(offsetof(KVMFRR, req) == KVMFR_R_LINE_SIZE * 2,
  "KVMFR recovery request header must be cache-line aligned");
static_assert(offsetof(KVMFRR, requests) == KVMFR_R_LINE_SIZE * 3,
  "KVMFR recovery request must be cache-line aligned");
static_assert(offsetof(KVMFRR, status) ==
    KVMFR_R_LINE_SIZE * (3 + KVMFR_R_REQ_SLOTS),
  "KVMFR recovery status must be cache-line aligned");
static_assert(sizeof(KVMFRR) ==
    KVMFR_R_LINE_SIZE * (4 + KVMFR_R_REQ_SLOTS),
  "KVMFR recovery layout changed");
static_assert(sizeof(KVMFRR) <= KVMFR_R_REGION_SIZE,
  "KVMFR recovery data must fit in its reserved region");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(KVMFR_R_REGION_SIZE % KVMFR_R_LINE_SIZE == 0,
  "KVMFR recovery region must contain whole cache lines");
_Static_assert(sizeof(KVMFRRHeader) == KVMFR_R_LINE_SIZE,
  "KVMFR recovery header must occupy one cache line");
_Static_assert(offsetof(KVMFRRHeader, lgmpVersion) == 16,
  "KVMFR recovery protocol version layout changed");
_Static_assert(offsetof(KVMFRRHeader, session) == 24,
  "KVMFR recovery session layout changed");
_Static_assert(offsetof(KVMFRRHeader, uuid) == 32,
  "KVMFR recovery UUID layout changed");
_Static_assert(offsetof(KVMFRRHeader, heartbeat) == 48,
  "KVMFR recovery heartbeat layout changed");
_Static_assert(offsetof(KVMFRRHeader, ready) == 60,
  "KVMFR recovery publication layout changed");
_Static_assert(sizeof(KVMFRRInfo) == KVMFR_R_LINE_SIZE,
  "KVMFR recovery producer information must occupy one cache line");
_Static_assert(sizeof(KVMFRRReqHead) == KVMFR_R_LINE_SIZE,
  "KVMFR recovery request header must occupy one cache line");
_Static_assert(sizeof(KVMFRRRequest) == KVMFR_R_LINE_SIZE,
  "KVMFR recovery request must occupy one cache line");
_Static_assert(offsetof(KVMFRRRequest, session) == 8,
  "KVMFR recovery request session layout changed");
_Static_assert(offsetof(KVMFRRRequest, request) == 4,
  "KVMFR recovery request publication layout changed");
_Static_assert(sizeof(KVMFRRStatus) == KVMFR_R_LINE_SIZE,
  "KVMFR recovery status must occupy one cache line");
_Static_assert(offsetof(KVMFRRStatus, session) == 16,
  "KVMFR recovery status session layout changed");
_Static_assert(offsetof(KVMFRRStatus, serial) == 24,
  "KVMFR recovery status publication layout changed");
_Static_assert(offsetof(KVMFRR, info) == KVMFR_R_LINE_SIZE,
  "KVMFR recovery producer information must be cache-line aligned");
_Static_assert(offsetof(KVMFRR, req) == KVMFR_R_LINE_SIZE * 2,
  "KVMFR recovery request header must be cache-line aligned");
_Static_assert(offsetof(KVMFRR, requests) == KVMFR_R_LINE_SIZE * 3,
  "KVMFR recovery request must be cache-line aligned");
_Static_assert(offsetof(KVMFRR, status) ==
    KVMFR_R_LINE_SIZE * (3 + KVMFR_R_REQ_SLOTS),
  "KVMFR recovery status must be cache-line aligned");
_Static_assert(sizeof(KVMFRR) ==
    KVMFR_R_LINE_SIZE * (4 + KVMFR_R_REQ_SLOTS),
  "KVMFR recovery layout changed");
_Static_assert(sizeof(KVMFRR) <= KVMFR_R_REGION_SIZE,
  "KVMFR recovery data must fit in its reserved region");
#endif

#endif
