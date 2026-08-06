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

#include "interface/transport.h"

#include "common/KVMFR.h"
#include "common/LGMPConfig.h"
#include "common/debug.h"
#include "common/ivshmem.h"
#include "common/locking.h"
#include "common/option.h"
#include "common/stringutils.h"

#include <lgmp/client.h>

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LGMP_TIMING_SPIN_COUNT 4096

struct DMAFrameInfo
{
  const KVMFRFrame * frame;
  size_t             dataSize;
  int                fd;
};

#define LGMP_FRAME_LEASE_COUNT (LGMP_Q_FRAME_LEN + 1)

struct LGMPFrameLease
{
  PLGMPClientQueue        * subscription;
  PLGMPClientQueue          queue;
  const KVMFRFrame        * frame;
  LG_TransportFrameFormat   format;
  uint64_t                  handle;
  uint32_t                  generation;
  bool                      releaseRequested;
  bool                      active;
};

struct LG_Transport
{
  struct IVSHMEM    shm;
  PLGMPClient       client;
  PLGMPClientQueue  frameQueue;
  PLGMPClientQueue  ownerFrameQueue[LGMP_Q_FRAME_LEN];
  PLGMPClientQueue  pointerQueue;
  LG_Lock           frameLock;
  LG_Lock           pointerLock;

  unsigned cursorPollInterval;
  unsigned framePollInterval;
  bool     allowDMA;
  bool     connected;
  bool     frameStopRequested;
  bool     frameScheduleSupported;
  uint64_t frameLeaseHandle;
  uint32_t frameGeneration;
  uint32_t clientID;
  uint32_t frameSerial;
  bool     frameSerialValid;
  bool     formatValid;
  LG_TransportFrameFormat format;
  struct LGMPFrameLease   frameLease[LGMP_FRAME_LEASE_COUNT];

  struct DMAFrameInfo dma[LGMP_Q_FRAME_BUFFER_LEN];
  uint8_t            * pointerData;
  size_t               pointerDataSize;
};

static bool lgmp_deviceValidator(struct Option * opt, const char ** error)
{
  const char * transport = option_get_string("app", "transport");
  if (!transport || strcmp(transport, "lgmp") != 0)
    return true;

  if (strlen(opt->value.x_string) > 3 &&
      memcmp(opt->value.x_string, "kvmfr", 5) != 0)
  {
    struct stat st;
    if (stat(opt->value.x_string, &st) != 0)
    {
      *error = "Invalid path to the shared memory file";
      return false;
    }
  }
  return true;
}

static StringList lgmp_deviceValues(struct Option * option)
{
  StringList values = stringlist_new(true);
  DIR * dir = opendir("/sys/class/kvmfr");
  if (!dir)
    return values;

  struct dirent * entry;
  while ((entry = readdir(dir)))
  {
    if (entry->d_name[0] == '.')
      continue;
    char * name;
    alloc_sprintf(&name, "/dev/%s", entry->d_name);
    stringlist_push(values, name);
  }
  closedir(dir);
  return values;
}

static void lgmp_setup(void)
{
  struct stat st;
  const char * defaultDevice = stat("/dev/kvmfr0", &st) == 0 ?
    "/dev/kvmfr0" : "/dev/shm/looking-glass";

  static struct Option options[] =
  {
    {
      .module         = "lgmp",
      .name           = "shmDevice",
      .old_module     = "app",
      .old_name       = "shmFile",
      .shortopt       = 'f',
      .description    = "Shared memory file or KVMFR device path",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = NULL,
      .validator      = lgmp_deviceValidator,
      .getValues      = lgmp_deviceValues,
    },
    {
      .module       = "lgmp",
      .name         = "allowDMA",
      .old_module   = "app",
      .old_name     = "allowDMA",
      .description  = "Allow direct DMA transfers when supported",
      .type         = OPTION_TYPE_BOOL,
      .value.x_bool = true,
    },
    {
      .module      = "lgmp",
      .name        = "framePollInterval",
      .old_module  = "app",
      .old_name    = "framePollInterval",
      .description = "Frame queue polling interval in microseconds",
      .type        = OPTION_TYPE_INT,
      .value.x_int = 1000,
    },
    {
      .module      = "lgmp",
      .name        = "cursorPollInterval",
      .old_module  = "app",
      .old_name    = "cursorPollInterval",
      .description = "Pointer queue polling interval in microseconds",
      .type        = OPTION_TYPE_INT,
      .value.x_int = 1000,
    },
    {0}
  };

  options[0].value.x_string = (char *)defaultDevice;
  option_register(options);
}

static bool lgmp_create(LG_Transport ** result)
{
  struct LG_Transport * this = calloc(1, sizeof(*this));
  if (!this)
    return false;

  this->allowDMA       = option_get_bool("lgmp", "allowDMA");
  const int framePoll  = option_get_int("lgmp", "framePollInterval");
  const int cursorPoll = option_get_int("lgmp", "cursorPollInterval");
  if (framePoll < 0 || cursorPoll < 0)
  {
    DEBUG_ERROR("LGMP polling intervals cannot be negative");
    free(this);
    return false;
  }
  this->framePollInterval  = framePoll;
  this->cursorPollInterval = cursorPoll;

  for (unsigned i = 0; i < LGMP_Q_FRAME_BUFFER_LEN; ++i)
    this->dma[i].fd = -1;

  LG_LOCK_INIT(this->frameLock);
  LG_LOCK_INIT(this->pointerLock);

  this->frameGeneration = 1;
  this->frameLease[0].subscription = &this->frameQueue;
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    this->frameLease[i + 1].subscription = &this->ownerFrameQueue[i];

  if (!ivshmemOpenDev(&this->shm,
        option_get_string("lgmp", "shmDevice")))
  {
    LG_LOCK_FREE(this->frameLock);
    LG_LOCK_FREE(this->pointerLock);
    free(this);
    return false;
  }

  LGMP_STATUS status = lgmpClientInit(this->shm.mem, this->shm.size,
      &this->client);
  if (status != LGMP_OK)
  {
    DEBUG_ERROR("lgmpClientInit failed: %s", lgmpStatusString(status));
    ivshmemClose(&this->shm);
    LG_LOCK_FREE(this->frameLock);
    LG_LOCK_FREE(this->pointerLock);
    free(this);
    return false;
  }

  *result = this;
  return true;
}

static void lgmp_unsubscribeFrameLease(struct LGMPFrameLease * lease)
{
  if (lease->active || !*lease->subscription)
    return;

  const LGMP_STATUS status = lgmpClientUnsubscribe(lease->subscription);
  if (status != LGMP_OK)
  {
    if (status != LGMP_ERR_QUEUE_TIMEOUT &&
        status != LGMP_ERR_QUEUE_UNSUBSCRIBED)
      DEBUG_WARN("Failed to unsubscribe from LGMP frame queue: %s",
          lgmpStatusString(status));
    *lease->subscription = NULL;
  }
}

static struct LGMPFrameLease * lgmp_findFrameLeaseLocked(
    struct LG_Transport * this, uint64_t handle)
{
  for (unsigned i = 0; i < LGMP_FRAME_LEASE_COUNT; ++i)
    if (this->frameLease[i].active &&
        this->frameLease[i].handle == handle)
      return &this->frameLease[i];
  return NULL;
}

static void lgmp_finishFrameLeaseLocked(struct LG_Transport * this,
    struct LGMPFrameLease * lease)
{
  if (lease->generation == this->frameGeneration && lease->queue &&
      *lease->subscription == lease->queue)
  {
    const LGMP_STATUS status = lgmpClientMessageDone(lease->queue);
    if (status == LGMP_ERR_INVALID_SESSION ||
        status == LGMP_ERR_QUEUE_TIMEOUT ||
        status == LGMP_ERR_QUEUE_UNSUBSCRIBED)
      *lease->subscription = NULL;
    else if (status != LGMP_OK)
      DEBUG_WARN("Failed to release LGMP frame: %s",
          lgmpStatusString(status));
  }

  lease->queue            = NULL;
  lease->frame            = NULL;
  lease->handle           = 0;
  lease->releaseRequested = false;
  lease->active           = false;
  if (this->frameStopRequested)
    lgmp_unsubscribeFrameLease(lease);
}

static void lgmp_drainFrameLeasesLocked(struct LG_Transport * this)
{
  for (unsigned i = 0; i < LGMP_FRAME_LEASE_COUNT; ++i)
    if (this->frameLease[i].active &&
        this->frameLease[i].releaseRequested)
      lgmp_finishFrameLeaseLocked(this, &this->frameLease[i]);
}

static void lgmp_stopFrameLocked(struct LG_Transport * this)
{
  this->frameStopRequested = true;
  /* stopFrame ends every lease by contract. The renderer is drained before
   * this call, while direct transport users may still hold a returned frame. */
  for (unsigned i = 0; i < LGMP_FRAME_LEASE_COUNT; ++i)
    if (this->frameLease[i].active)
      lgmp_finishFrameLeaseLocked(this, &this->frameLease[i]);
  for (unsigned i = 0; i < LGMP_FRAME_LEASE_COUNT; ++i)
    lgmp_unsubscribeFrameLease(&this->frameLease[i]);

  this->frameSerial      = 0;
  this->frameSerialValid = false;
  this->formatValid      = false;
}

static void lgmp_stopFrame(struct LG_Transport * this)
{
  LG_LOCK(this->frameLock);
  lgmp_stopFrameLocked(this);
  LG_UNLOCK(this->frameLock);
}

static void lgmp_stopPointer(struct LG_Transport * this)
{
  LG_LOCK(this->pointerLock);
  const LGMP_STATUS status = lgmpClientUnsubscribe(&this->pointerQueue);
  if (status != LGMP_OK)
  {
    if (status != LGMP_ERR_QUEUE_TIMEOUT &&
        status != LGMP_ERR_QUEUE_UNSUBSCRIBED)
      DEBUG_WARN("Failed to unsubscribe from the LGMP pointer queue: %s",
          lgmpStatusString(status));
    this->pointerQueue = NULL;
  }
  LG_UNLOCK(this->pointerLock);
}

static void lgmp_closeQueues(struct LG_Transport * this)
{
  lgmp_stopFrame(this);
  lgmp_stopPointer(this);
}

static void lgmp_closeDMA(struct LG_Transport * this)
{
  for (unsigned i = 0; i < LGMP_Q_FRAME_BUFFER_LEN; ++i)
  {
    if (this->dma[i].fd >= 0)
      close(this->dma[i].fd);
    this->dma[i].fd       = -1;
    this->dma[i].frame    = NULL;
    this->dma[i].dataSize = 0;
  }
}

static void lgmp_destroy(LG_Transport ** transport)
{
  if (!transport || !*transport)
    return;

  struct LG_Transport * this = *transport;
  lgmp_closeQueues(this);
  lgmp_closeDMA(this);
  free(this->pointerData);
  lgmpClientFree(&this->client);
  ivshmemClose(&this->shm);
  LG_LOCK_FREE(this->frameLock);
  LG_LOCK_FREE(this->pointerLock);
  free(this);
  *transport = NULL;
}

static bool lgmp_parseSession(const uint8_t * data, uint32_t size,
    LG_TransportSession * session)
{
  if (size < sizeof(KVMFR))
    return false;

  const KVMFR * header = (const KVMFR *)data;
  if (memcmp(header->magic, KVMFR_MAGIC, sizeof(header->magic)) != 0 ||
      header->version != KVMFR_VERSION)
  {
    session->remoteVersion = header->version;
    return false;
  }

  str_copy(session->version, sizeof(session->version), header->hostver,
      sizeof(header->hostver));
  if (header->features & KVMFR_FEATURE_SETCURSORPOS)
    session->features |= LG_TRANSPORT_FEATURE_SET_CURSOR_POS;
  if (header->features & KVMFR_FEATURE_WINDOWSIZE)
    session->features |= LG_TRANSPORT_FEATURE_WINDOW_SIZE;
  if (header->features & KVMFR_FEATURE_FRAME_SCHEDULE)
    session->features |= LG_TRANSPORT_FEATURE_FRAME_SCHEDULE;

  data += sizeof(*header);
  size -= sizeof(*header);
  while (size >= sizeof(KVMFRRecord))
  {
    const KVMFRRecord * record = (const KVMFRRecord *)data;
    data += sizeof(*record);
    size -= sizeof(*record);
    if (record->size > size)
      break;

    switch (record->type)
    {
      case KVMFR_RECORD_VMINFO:
        if (record->size >= sizeof(KVMFRRecord_VMInfo))
        {
          const KVMFRRecord_VMInfo * info =
            (const KVMFRRecord_VMInfo *)data;
          memcpy(session->uuid, info->uuid, sizeof(session->uuid));
          for (unsigned i = 0; i < sizeof(session->uuid); ++i)
            session->uuidValid |= session->uuid[i] != 0;
          str_copy(session->capture, sizeof(session->capture), info->capture,
              sizeof(info->capture));
          session->cpus    = info->cpus;
          session->cores   = info->cores;
          session->sockets = info->sockets;
          const size_t fixed = offsetof(KVMFRRecord_VMInfo, model);
          str_copy(session->cpuModel, sizeof(session->cpuModel), info->model,
              record->size - fixed);
        }
        break;

      case KVMFR_RECORD_OSINFO:
        if (record->size >= sizeof(KVMFRRecord_OSInfo))
        {
          const KVMFRRecord_OSInfo * info =
            (const KVMFRRecord_OSInfo *)data;
          session->os = info->os <= KVMFR_OS_OTHER ?
            (LG_TransportGuestOS)info->os : LG_TRANSPORT_OS_OTHER;
          const size_t fixed = offsetof(KVMFRRecord_OSInfo, name);
          str_copy(session->osName, sizeof(session->osName), info->name,
              record->size - fixed);
        }
        break;
    }

    data += record->size;
    size -= record->size;
  }

  return true;
}

static LG_TransportStatus lgmp_connect(LG_Transport * this,
    LG_TransportSession * session)
{
  memset(session, 0, sizeof(*session));
  session->os = LG_TRANSPORT_OS_OTHER;

  uint32_t size;
  uint8_t * data;
  uint32_t remoteVersion;
  LGMP_STATUS status = lgmpClientSessionInit(this->client, &size, &data,
      &this->clientID, &remoteVersion);
  session->remoteVersion = remoteVersion;
  switch (status)
  {
    case LGMP_OK:
      if (!lgmp_parseSession(data, size, session))
        return LG_TRANSPORT_INVALID_VERSION;

      LG_LOCK(this->frameLock);
      if (++this->frameGeneration == 0)
        ++this->frameGeneration;
      this->connected              = true;
      this->frameStopRequested     = false;
      this->frameScheduleSupported =
        session->features & LG_TRANSPORT_FEATURE_FRAME_SCHEDULE;
      this->frameSerial            = 0;
      this->frameSerialValid       = false;
      this->formatValid            = false;
      LG_UNLOCK(this->frameLock);
      return LG_TRANSPORT_OK;

    case LGMP_ERR_INVALID_VERSION:
      return LG_TRANSPORT_INVALID_VERSION;

    case LGMP_ERR_INVALID_SESSION:
    case LGMP_ERR_INVALID_MAGIC:
      return LG_TRANSPORT_UNAVAILABLE;

    default:
      DEBUG_ERROR("lgmpClientSessionInit failed: %s", lgmpStatusString(status));
      return LG_TRANSPORT_ERROR;
  }
}

static void lgmp_disconnect(LG_Transport * this)
{
  lgmp_closeQueues(this);

  LG_LOCK(this->frameLock);
  if (++this->frameGeneration == 0)
    ++this->frameGeneration;
  this->frameQueue = NULL;
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    this->ownerFrameQueue[i] = NULL;
  this->connected              = false;
  this->frameScheduleSupported = false;
  this->clientID               = 0;
  LG_UNLOCK(this->frameLock);

  lgmp_closeDMA(this);
}

static bool lgmp_sessionValid(LG_Transport * this)
{
  return this->connected && lgmpClientSessionValid(this->client);
}

static bool lgmp_supportsDMA(LG_Transport * this)
{
  return this->allowDMA && ivshmemHasDMA(&this->shm);
}

static bool lgmp_attachRenderer(LG_Transport * this,
    const LG_RendererInterop * interop)
{
  return true;
}

static void lgmp_detachRenderer(LG_Transport * this)
{
}

static LG_TransportStatus lgmp_subscribe(PLGMPClient client, uint32_t id,
    PLGMPClientQueue * queue)
{
  if (*queue)
    return LG_TRANSPORT_OK;

  PLGMPClientQueue subscribed = NULL;
  LGMP_STATUS status = lgmpClientSubscribe(client, id, &subscribed);
  switch (status)
  {
    case LGMP_OK:
      *queue = subscribed;
      return LG_TRANSPORT_OK;
    case LGMP_ERR_NO_SUCH_QUEUE: return LG_TRANSPORT_TIMEOUT;
    case LGMP_ERR_INVALID_SESSION: return LG_TRANSPORT_DISCONNECTED;
    default:
      DEBUG_ERROR("lgmpClientSubscribe failed: %s", lgmpStatusString(status));
      return LG_TRANSPORT_ERROR;
  }
}

static LG_TransportStatus lgmp_process(PLGMPClientQueue * subscription,
    unsigned interval, LGMPMessage * message)
{
  PLGMPClientQueue queue = *subscription;
  if (!queue)
    return LG_TRANSPORT_TIMEOUT;

  LGMP_STATUS status = lgmpClientProcess(queue, message);
  switch (status)
  {
    case LGMP_OK:
      return LG_TRANSPORT_OK;
    case LGMP_ERR_QUEUE_EMPTY:
      if (interval)
        usleep(interval);
      return LG_TRANSPORT_TIMEOUT;
    case LGMP_ERR_QUEUE_TIMEOUT:
    case LGMP_ERR_QUEUE_UNSUBSCRIBED:
      *subscription = NULL;
      return LG_TRANSPORT_TIMEOUT;
    case LGMP_ERR_INVALID_SESSION:
      *subscription = NULL;
      return LG_TRANSPORT_DISCONNECTED;
    default:
      DEBUG_ERROR("lgmpClientProcess failed: %s", lgmpStatusString(status));
      return LG_TRANSPORT_ERROR;
  }
}

struct LGMPFrameMessage
{
  PLGMPClientQueue         queue;
  LGMPMessage              message;
  const KVMFRFrame       * frame;
  struct LGMPFrameLease  * lease;
  bool                     owner;
};

static LG_TransportStatus lgmp_pollFrameQueue(
    PLGMPClientQueue * subscription,
    bool owner, struct LGMPFrameMessage * result)
{
  PLGMPClientQueue queue = *subscription;
  if (!queue)
    return LG_TRANSPORT_TIMEOUT;

  const LGMP_STATUS advance = lgmpClientAdvanceToLast(queue);
  switch (advance)
  {
    case LGMP_OK:
      break;
    case LGMP_ERR_QUEUE_EMPTY:
      break;
    case LGMP_ERR_QUEUE_TIMEOUT:
    case LGMP_ERR_QUEUE_UNSUBSCRIBED:
      *subscription = NULL;
      return LG_TRANSPORT_TIMEOUT;
    case LGMP_ERR_INVALID_SESSION:
      *subscription = NULL;
      return LG_TRANSPORT_DISCONNECTED;
    default:
      DEBUG_ERROR("lgmpClientAdvanceToLast failed: %s",
          lgmpStatusString(advance));
      return LG_TRANSPORT_ERROR;
  }

  const LG_TransportStatus status =
    lgmp_process(subscription, 0, &result->message);
  if (status == LG_TRANSPORT_OK)
  {
    result->queue = queue;
    result->owner = owner || result->message.udata != 0;
  }
  return status;
}

static LG_TransportStatus lgmp_doneFrameMessage(
    struct LGMPFrameMessage * message)
{
  if (!message || !message->queue)
    return LG_TRANSPORT_OK;

  const LGMP_STATUS status = lgmpClientMessageDone(message->queue);
  message->queue = NULL;
  message->frame = NULL;

  if (status == LGMP_OK)
    return LG_TRANSPORT_OK;
  if (status == LGMP_ERR_INVALID_SESSION)
  {
    *message->lease->subscription = NULL;
    return LG_TRANSPORT_DISCONNECTED;
  }
  if (status == LGMP_ERR_QUEUE_TIMEOUT ||
      status == LGMP_ERR_QUEUE_UNSUBSCRIBED)
  {
    *message->lease->subscription = NULL;
    return LG_TRANSPORT_OK;
  }

  DEBUG_WARN("Failed to release discarded LGMP frame: %s",
      lgmpStatusString(status));
  return LG_TRANSPORT_ERROR;
}

static void lgmp_clearPointerQueue(LG_Transport * this,
    PLGMPClientQueue expected)
{
  LG_LOCK(this->pointerLock);
  if (this->pointerQueue == expected)
    this->pointerQueue = NULL;
  LG_UNLOCK(this->pointerLock);
}

static LG_TransportStatus lgmp_donePointerMessage(LG_Transport * this,
    PLGMPClientQueue queue)
{
  const LGMP_STATUS status = lgmpClientMessageDone(queue);
  if (status == LGMP_OK)
    return LG_TRANSPORT_OK;
  if (status == LGMP_ERR_INVALID_SESSION)
  {
    lgmp_clearPointerQueue(this, queue);
    return LG_TRANSPORT_DISCONNECTED;
  }
  if (status == LGMP_ERR_QUEUE_TIMEOUT ||
      status == LGMP_ERR_QUEUE_UNSUBSCRIBED)
  {
    lgmp_clearPointerQueue(this, queue);
    return LG_TRANSPORT_OK;
  }

  DEBUG_WARN("Failed to release LGMP pointer message: %s",
      lgmpStatusString(status));
  return LG_TRANSPORT_ERROR;
}

static void lgmp_mergeFrameStatus(LG_TransportStatus status,
    LG_TransportStatus * result)
{
  if (status != LG_TRANSPORT_OK &&
      (*result == LG_TRANSPORT_OK ||
       status == LG_TRANSPORT_DISCONNECTED))
    *result = status;
}

static bool lgmp_validateFrameMessage(struct LGMPFrameMessage * message)
{
  if (message->message.size < sizeof(KVMFRFrame))
  {
    DEBUG_ERROR("LGMP frame payload is too small");
    return false;
  }

  const KVMFRFrame * frame = (const KVMFRFrame *)message->message.mem;
  const size_t frameDataSize = (size_t)frame->dataHeight * frame->pitch;
  if (frame->type <= FRAME_TYPE_INVALID || frame->type >= FRAME_TYPE_MAX ||
      frame->offset > message->message.size - sizeof(FrameBuffer) ||
      frameDataSize >
        message->message.size - frame->offset - sizeof(FrameBuffer))
  {
    DEBUG_ERROR("LGMP frame payload contains invalid dimensions or offsets");
    return false;
  }

  message->frame = frame;
  return true;
}

static bool lgmp_frameSerialNewer(uint32_t lhs, uint32_t rhs)
{
  return lhs != rhs &&
    (uint32_t)(lhs - rhs) < UINT32_C(0x80000000);
}

static bool lgmp_frameMessageHasMatchingDeadline(
    const struct LGMPFrameMessage * message)
{
  if (!message->owner)
    return false;

  const uint32_t epoch = (uint32_t)(message->message.udata >> 32);
  const uint32_t deadlineSerial = (uint32_t)message->message.udata;
  return deadlineSerial && epoch == message->frame->scheduleEpoch &&
    deadlineSerial == message->frame->scheduleDeadlineSerial;
}

static void lgmp_selectNewestFrameMessage(
    struct LGMPFrameMessage * candidate,
    struct LGMPFrameMessage ** selected)
{
  if (!candidate->frame)
    return;

  if (!*selected ||
      lgmp_frameSerialNewer(candidate->frame->frameSerial,
        (*selected)->frame->frameSerial) ||
      (candidate->frame->frameSerial == (*selected)->frame->frameSerial &&
       ((candidate->owner && !(*selected)->owner) ||
        (lgmp_frameMessageHasMatchingDeadline(candidate) &&
         !lgmp_frameMessageHasMatchingDeadline(*selected)))))
    *selected = candidate;
}

static int lgmp_getDMA(struct LG_Transport * this, const KVMFRFrame * frame,
    size_t dataSize)
{
  struct DMAFrameInfo * dma = NULL;
  for (unsigned i = 0; i < LGMP_Q_FRAME_BUFFER_LEN; ++i)
    if (this->dma[i].frame == frame)
    {
      dma = &this->dma[i];
      if (dma->dataSize < dataSize && dma->fd >= 0)
      {
        close(dma->fd);
        dma->fd = -1;
      }
      break;
    }

  if (!dma)
    for (unsigned i = 0; i < LGMP_Q_FRAME_BUFFER_LEN; ++i)
      if (!this->dma[i].frame)
      {
        dma = &this->dma[i];
        dma->frame = frame;
        break;
      }

  if (!dma)
    return -1;
  if (dma->fd >= 0)
    return dma->fd;

  const uintptr_t position = (uintptr_t)frame - (uintptr_t)this->shm.mem;
  const uintptr_t offset = frame->offset + sizeof(FrameBuffer);
  dma->dataSize = dataSize;
  dma->fd       = ivshmemGetDMABuf(&this->shm, position + offset, dataSize);
  return dma->fd;
}

static void lgmp_releaseFrameLease(void * opaque, uint64_t handle);

static LG_TransportStatus lgmp_nextFrameLocked(LG_Transport * this,
    bool useDMA,
    LG_TransportFrame * result)
{
  lgmp_drainFrameLeasesLocked(this);
  if (!this->connected)
    return LG_TRANSPORT_DISCONNECTED;

  this->frameStopRequested = false;

  if (this->frameScheduleSupported)
    for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    {
      const LG_TransportStatus status = lgmp_subscribe(this->client,
          LGMP_Q_FRAME_OWNER + i, &this->ownerFrameQueue[i]);
      if (status != LG_TRANSPORT_OK && status != LG_TRANSPORT_TIMEOUT)
        return status;
    }

  const LG_TransportStatus sharedSubscribe = lgmp_subscribe(this->client,
      LGMP_Q_FRAME, &this->frameQueue);
  if (sharedSubscribe != LG_TRANSPORT_OK &&
      sharedSubscribe != LG_TRANSPORT_TIMEOUT)
    return sharedSubscribe;

  struct LGMPFrameMessage owner[LGMP_Q_FRAME_LEN] = {};
  struct LGMPFrameMessage shared = {};
  LG_TransportStatus sharedStatus = LG_TRANSPORT_TIMEOUT;
  LG_TransportStatus ownerStatus[LGMP_Q_FRAME_LEN];

  /* Select the newest frame across the shared and owner delivery lanes. */
  shared.lease = &this->frameLease[0];
  if (this->frameQueue && !shared.lease->active)
    sharedStatus = lgmp_pollFrameQueue(&this->frameQueue, false, &shared);
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    owner[i].lease = &this->frameLease[i + 1];
    ownerStatus[i] = LG_TRANSPORT_TIMEOUT;
    if (this->ownerFrameQueue[i] && !owner[i].lease->active)
      ownerStatus[i] = lgmp_pollFrameQueue(
          &this->ownerFrameQueue[i], true, &owner[i]);
  }

  LG_TransportStatus failure = LG_TRANSPORT_OK;
  if (sharedStatus != LG_TRANSPORT_OK &&
      sharedStatus != LG_TRANSPORT_TIMEOUT)
    failure = sharedStatus;
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    if (ownerStatus[i] != LG_TRANSPORT_OK &&
        ownerStatus[i] != LG_TRANSPORT_TIMEOUT &&
        (failure == LG_TRANSPORT_OK ||
         ownerStatus[i] == LG_TRANSPORT_DISCONNECTED))
      failure = ownerStatus[i];

  if (failure != LG_TRANSPORT_OK)
  {
    lgmp_mergeFrameStatus(
        lgmp_doneFrameMessage(&shared), &failure);
    for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
      lgmp_mergeFrameStatus(
          lgmp_doneFrameMessage(&owner[i]), &failure);
    return failure;
  }

  bool malformed = false;
  LG_TransportStatus releaseFailure = LG_TRANSPORT_OK;
  if (sharedStatus == LG_TRANSPORT_OK &&
      !lgmp_validateFrameMessage(&shared))
  {
    malformed = true;
    releaseFailure = lgmp_doneFrameMessage(&shared);
  }
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    if (ownerStatus[i] == LG_TRANSPORT_OK &&
        !lgmp_validateFrameMessage(&owner[i]))
    {
      malformed = true;
      const LG_TransportStatus done =
        lgmp_doneFrameMessage(&owner[i]);
      lgmp_mergeFrameStatus(done, &releaseFailure);
    }

  if (releaseFailure != LG_TRANSPORT_OK)
  {
    lgmp_mergeFrameStatus(
        lgmp_doneFrameMessage(&shared), &releaseFailure);
    for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
      lgmp_mergeFrameStatus(
          lgmp_doneFrameMessage(&owner[i]), &releaseFailure);
    return releaseFailure;
  }

  struct LGMPFrameMessage * selected = NULL;
  lgmp_selectNewestFrameMessage(&shared, &selected);
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    lgmp_selectNewestFrameMessage(&owner[i], &selected);

  if (!selected)
    return malformed ? LG_TRANSPORT_ERROR : LG_TRANSPORT_TIMEOUT;

  if (selected != &shared)
    releaseFailure = lgmp_doneFrameMessage(&shared);
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    if (selected != &owner[i])
    {
      const LG_TransportStatus done =
        lgmp_doneFrameMessage(&owner[i]);
      lgmp_mergeFrameStatus(done, &releaseFailure);
    }

  if (releaseFailure != LG_TRANSPORT_OK)
  {
    lgmp_mergeFrameStatus(
        lgmp_doneFrameMessage(selected), &releaseFailure);
    return releaseFailure;
  }

  const KVMFRFrame * frame = selected->frame;
  const bool equalSerial = this->frameSerialValid &&
    frame->frameSerial == this->frameSerial;
  if (this->frameSerialValid && !equalSerial &&
      !lgmp_frameSerialNewer(frame->frameSerial, this->frameSerial))
  {
    const LG_TransportStatus done = lgmp_doneFrameMessage(selected);
    return done == LG_TRANSPORT_OK ? LG_TRANSPORT_TIMEOUT : done;
  }
  if (equalSerial && !selected->owner)
  {
    const LG_TransportStatus done = lgmp_doneFrameMessage(selected);
    return done == LG_TRANSPORT_OK ? LG_TRANSPORT_TIMEOUT : done;
  }

  const bool fullDamage = !this->frameSerialValid || equalSerial ||
    frame->frameSerial != this->frameSerial + 1;
  this->frameSerial      = frame->frameSerial;
  this->frameSerialValid = true;

  memset(result, 0, sizeof(*result));
  result->serial = frame->frameSerial;
  if (selected->owner)
  {
    const uint64_t scheduleToken = selected->message.udata;
    result->scheduleEpoch          = (uint32_t)(scheduleToken >> 32);
    result->scheduleDeadlineSerial = (uint32_t)scheduleToken;
    result->scheduleOwner          = true;
  }
  if (frame->flags & FRAME_FLAG_BLOCK_SCREENSAVER)
    result->flags |= LG_TRANSPORT_FRAME_BLOCK_SCREENSAVER;
  if (frame->flags & FRAME_FLAG_REQUEST_ACTIVATION)
    result->flags |= LG_TRANSPORT_FRAME_REQUEST_ACTIVATION;
  if (frame->flags & FRAME_FLAG_TRUNCATED)
    result->flags |= LG_TRANSPORT_FRAME_TRUNCATED;

  LG_TransportFrameFormat * format = &this->format;
  if (!this->formatValid || format->version != frame->formatVer)
  {
    memset(format, 0, sizeof(*format));
    format->version       = frame->formatVer;
    format->type          = frame->type;
    format->screenWidth   = frame->screenWidth;
    format->screenHeight  = frame->screenHeight;
    format->dataWidth     = frame->dataWidth;
    format->dataHeight    = frame->dataHeight;
    format->frameWidth    = frame->frameWidth;
    format->frameHeight   = frame->frameHeight;
    format->rotation      = frame->rotation;
    format->stride        = frame->stride;
    format->pitch         = frame->pitch;
    format->hdr           = frame->flags & FRAME_FLAG_HDR;
    format->hdrPQ         = frame->flags & FRAME_FLAG_HDR_PQ;
    format->hdrMetadata   = frame->flags & FRAME_FLAG_HDR_METADATA;
    format->sdrWhiteLevel = frame->sdrWhiteLevel ? frame->sdrWhiteLevel :
      KVMFR_SDR_WHITE_LEVEL_DEFAULT;
    if (format->hdrMetadata)
    {
      memcpy(format->hdrDisplayPrimary, frame->hdrDisplayPrimary,
          sizeof(format->hdrDisplayPrimary));
      memcpy(format->hdrWhitePoint, frame->hdrWhitePoint,
          sizeof(format->hdrWhitePoint));
      format->hdrMaxDisplayLuminance       = frame->hdrMaxDisplayLuminance;
      format->hdrMinDisplayLuminance       = frame->hdrMinDisplayLuminance;
      format->hdrMaxContentLightLevel      = frame->hdrMaxContentLightLevel;
      format->hdrMaxFrameAverageLightLevel =
        frame->hdrMaxFrameAverageLightLevel;
    }
    this->formatValid = true;
  }
  struct LGMPFrameLease * lease = selected->lease;
  memcpy(&lease->format, format, sizeof(lease->format));
  result->format = &lease->format;

  result->framebuffer = (const FrameBuffer *)((const uint8_t *)frame +
      frame->offset);
  result->dmaFD       = -1;
  if (useDMA)
  {
    const size_t dataSize = (size_t)format->dataHeight * format->pitch;
    result->dmaFD = lgmp_getDMA(this, frame, dataSize);
    if (result->dmaFD < 0)
    {
      const LG_TransportStatus done = lgmp_doneFrameMessage(selected);
      DEBUG_ERROR("Failed to obtain a DMA buffer for the frame");
      return done == LG_TRANSPORT_DISCONNECTED ?
        done : LG_TRANSPORT_ERROR;
    }
  }

  if (!fullDamage && frame->damageRectsCount <= KVMFR_MAX_DAMAGE_RECTS)
  {
    result->damageRects      = frame->damageRects;
    result->damageRectsCount = frame->damageRectsCount;
  }
  else if (!fullDamage)
    DEBUG_WARN("Invalid damage rectangles, forcing a full update");

  lease->queue = selected->queue;
  lease->frame = frame;
  if (++this->frameLeaseHandle == 0)
    ++this->frameLeaseHandle;
  lease->handle           = this->frameLeaseHandle;
  lease->generation       = this->frameGeneration;
  lease->releaseRequested = false;
  lease->active           = true;
  result->releaseFn     = lgmp_releaseFrameLease;
  result->releaseOpaque = this;
  result->releaseHandle = lease->handle;
  return LG_TRANSPORT_OK;
}

static LG_TransportStatus lgmp_nextFrame(LG_Transport * this, bool useDMA,
    LG_TransportFrame * result)
{
  LG_LOCK(this->frameLock);
  const LG_TransportStatus status =
    lgmp_nextFrameLocked(this, useDMA, result);
  LG_UNLOCK(this->frameLock);
  if ((status == LG_TRANSPORT_TIMEOUT ||
       status == LG_TRANSPORT_UNAVAILABLE) && this->framePollInterval)
    usleep(this->framePollInterval);
  return status;
}

static bool lgmp_frameTimingReady(const KVMFRFrame * frame)
{
  return __atomic_load_n(&frame->timingValid, __ATOMIC_ACQUIRE) &&
    frame->timingSerial == frame->frameSerial;
}

static void lgmp_getFrameTiming(LG_Transport * this,
    const LG_TransportFrame * frame, LG_TransportFrameTiming * timing)
{
  memset(timing, 0, sizeof(*timing));

  LG_LOCK(this->frameLock);
  struct LGMPFrameLease * lease =
    lgmp_findFrameLeaseLocked(this, frame->releaseHandle);

  if (!lease ||
      lease->generation != this->frameGeneration || !lease->frame ||
      lease->frame->frameSerial != frame->serial)
  {
    LG_UNLOCK(this->frameLock);
    return;
  }

  /* The producer writes these immediately after publishing FrameBuffer::wp.
   * nextFrame can observe the header earlier, so briefly observe the
   * publication tail after onFrame consumes the framebuffer without sleeping
   * the frame-acquisition thread. */
  for (unsigned i = 0;
       !lgmp_frameTimingReady(lease->frame) &&
       i < LGMP_TIMING_SPIN_COUNT;
       ++i)
  {
  }

  if (!lgmp_frameTimingReady(lease->frame))
  {
    LG_UNLOCK(this->frameLock);
    return;
  }

  timing->valid                  = true;
  timing->scheduleGeneration     = lease->frame->scheduleGeneration;
  timing->scheduleEpoch          = lease->frame->scheduleEpoch;
  timing->scheduleDeadlineSerial = lease->frame->scheduleDeadlineSerial;
  timing->phaseValid             = frame->scheduleOwner &&
    timing->scheduleGeneration && timing->scheduleEpoch &&
    timing->scheduleDeadlineSerial &&
    timing->scheduleEpoch == frame->scheduleEpoch &&
    timing->scheduleDeadlineSerial == frame->scheduleDeadlineSerial &&
    (lease->frame->timingFlags & KVMFR_FRAME_TIMING_PHASE_VALID);
  timing->captureTime            = lease->frame->captureTime;
  timing->postProcessTime        = lease->frame->postProcessTime;
  timing->copyTime               = lease->frame->copyTime;
  timing->readyTime              = lease->frame->readyTime;
  LG_UNLOCK(this->frameLock);
}

static void lgmp_releaseFrameLease(void * opaque, uint64_t handle)
{
  LG_Transport * this = opaque;

  LG_LOCK(this->frameLock);
  struct LGMPFrameLease * lease = lgmp_findFrameLeaseLocked(this, handle);
  if (lease)
    lease->releaseRequested = true;
  LG_UNLOCK(this->frameLock);
}

static void lgmp_releaseFrame(LG_Transport * this, LG_TransportFrame * frame)
{
  if (frame->releaseFn)
  {
    LG_LOCK(this->frameLock);
    struct LGMPFrameLease * lease =
      lgmp_findFrameLeaseLocked(this, frame->releaseHandle);
    if (lease)
      lgmp_finishFrameLeaseLocked(this, lease);
    LG_UNLOCK(this->frameLock);

    frame->releaseFn     = NULL;
    frame->releaseOpaque = NULL;
    frame->releaseHandle = 0;
  }
  memset(frame, 0, sizeof(*frame));
}

static LG_TransportStatus lgmp_nextPointer(LG_Transport * this,
    LG_TransportPointer * result)
{
  if (!this->connected)
    return LG_TRANSPORT_DISCONNECTED;

  uint32_t           pointerFlags  = 0;
  size_t             shapeSize     = 0;
  size_t             transformSize = 0;
  LG_TransportStatus status        = LG_TRANSPORT_OK;
  PLGMPClientQueue   pointerQueue;
  LG_LOCK(this->pointerLock);
  if (!this->pointerQueue)
  {
    status = lgmp_subscribe(this->client, LGMP_Q_POINTER,
        &this->pointerQueue);
  }
  pointerQueue = this->pointerQueue;
  LG_UNLOCK(this->pointerLock);
  if (status == LG_TRANSPORT_TIMEOUT)
    usleep(1000);
  if (status == LG_TRANSPORT_OK)
  {
    LGMPMessage message;
    PLGMPClientQueue processedQueue = pointerQueue;
    status = lgmp_process(&processedQueue, this->cursorPollInterval,
        &message);
    if (!processedQueue)
      lgmp_clearPointerQueue(this, pointerQueue);
    if (status == LG_TRANSPORT_OK)
    {
      if (message.size < sizeof(KVMFRCursor))
      {
        const LG_TransportStatus done =
          lgmp_donePointerMessage(this, pointerQueue);
        status = done == LG_TRANSPORT_DISCONNECTED ?
          done : LG_TRANSPORT_ERROR;
      }
      else
      {
        const KVMFRCursor * cursor = (const KVMFRCursor *)message.mem;
        shapeSize = message.udata & CURSOR_FLAG_SHAPE ?
          (size_t)cursor->height * cursor->pitch : 0;
        transformSize =
          message.udata & CURSOR_FLAG_COLOR_TRANSFORM ?
            sizeof(KVMFRColorTransform) : 0;
        if (shapeSize > message.size - sizeof(*cursor) ||
            transformSize > message.size - sizeof(*cursor) - shapeSize)
        {
          const LG_TransportStatus done =
            lgmp_donePointerMessage(this, pointerQueue);
          status = done == LG_TRANSPORT_DISCONNECTED ?
            done : LG_TRANSPORT_ERROR;
        }
        else
        {
          const size_t needed = sizeof(*cursor) + shapeSize + transformSize;
          if (needed > this->pointerDataSize)
          {
            void * data = realloc(this->pointerData, needed);
            if (!data)
              status = LG_TRANSPORT_ERROR;
            else
            {
              this->pointerData     = data;
              this->pointerDataSize = needed;
            }
          }
          if (status == LG_TRANSPORT_OK)
          {
            memcpy(this->pointerData, message.mem, needed);
            pointerFlags = (uint32_t)message.udata;
          }
          const LG_TransportStatus done =
            lgmp_donePointerMessage(this, pointerQueue);
          if (status == LG_TRANSPORT_OK)
            status = done;
        }
      }
    }
  }
  if (status != LG_TRANSPORT_OK)
    return status;

  const KVMFRCursor * cursor = (const KVMFRCursor *)this->pointerData;
  memset(result, 0, sizeof(*result));
  if (pointerFlags & CURSOR_FLAG_POSITION)
    result->flags |= LG_TRANSPORT_POINTER_POSITION;
  if (pointerFlags & CURSOR_FLAG_VISIBLE)
    result->flags |= LG_TRANSPORT_POINTER_VISIBLE;
  if (pointerFlags & CURSOR_FLAG_SHAPE)
    result->flags |= LG_TRANSPORT_POINTER_SHAPE;
  if (pointerFlags & CURSOR_FLAG_COLOR_TRANSFORM)
    result->flags |= LG_TRANSPORT_POINTER_COLOR_TRANSFORM;
  if (pointerFlags & CURSOR_FLAG_VISIBLE_VALID)
    result->flags |= LG_TRANSPORT_POINTER_VISIBLE_VALID;
  result->x             = cursor->x;
  result->y             = cursor->y;
  result->type          = cursor->type;
  result->hx            = cursor->hx;
  result->hy            = cursor->hy;
  result->width         = cursor->width;
  result->height        = cursor->height;
  result->pitch         = cursor->pitch;
  result->sdrWhiteLevel = cursor->sdrWhiteLevel;
  result->shape         = (const uint8_t *)(cursor + 1);
  if (transformSize)
    result->colorTransform = (const LGColorTransform *)(result->shape +
        shapeSize);
  return LG_TRANSPORT_OK;
}

static void lgmp_releasePointer(LG_Transport * this,
    LG_TransportPointer * pointer)
{
  memset(pointer, 0, sizeof(*pointer));
}

static LG_TransportStatus lgmp_sendControl(LG_Transport * this,
    const LG_TransportControl * control, LG_TransportControlToken * token)
{
  uint8_t buffer[LGMP_MSGS_SIZE];
  uint32_t size;
  switch (control->type)
  {
    case LG_TRANSPORT_CONTROL_SET_CURSOR_POS:
    {
      const KVMFRSetCursorPos message = {
        .msg.type = KVMFR_MESSAGE_SETCURSORPOS,
        .x        = control->cursorPos.x,
        .y        = control->cursorPos.y,
      };
      memcpy(buffer, &message, sizeof(message));
      size = sizeof(message);
      break;
    }
    case LG_TRANSPORT_CONTROL_WINDOW_SIZE:
    {
      const KVMFRWindowSize message = {
        .msg.type = KVMFR_MESSAGE_WINDOWSIZE,
        .w        = control->windowSize.width,
        .h        = control->windowSize.height,
      };
      memcpy(buffer, &message, sizeof(message));
      size = sizeof(message);
      break;
    }
    case LG_TRANSPORT_CONTROL_FRAME_SCHEDULE:
    {
      const KVMFRFrameSchedule message = {
        .msg.type               = KVMFR_MESSAGE_FRAME_SCHEDULE,
        .clientID               = this->clientID,
        .generation             = control->frameSchedule.generation,
        .flags                  = control->frameSchedule.flags,
        .period                 = control->frameSchedule.period,
        .targetSlack            = control->frameSchedule.targetSlack,
        .phaseError             = control->frameSchedule.phaseError,
        .feedbackFrameSerial    = control->frameSchedule.feedbackFrameSerial,
        .feedbackScheduleEpoch  =
          control->frameSchedule.feedbackScheduleEpoch,
        .feedbackDeadlineSerial =
          control->frameSchedule.feedbackDeadlineSerial,
        .lease                  = control->frameSchedule.lease,
      };
      memcpy(buffer, &message, sizeof(message));
      size = sizeof(message);
      break;
    }
    default:
      return LG_TRANSPORT_ERROR;
  }

  LG_LOCK(this->pointerLock);
  if (!this->pointerQueue)
  {
    LG_UNLOCK(this->pointerLock);
    return LG_TRANSPORT_UNAVAILABLE;
  }
  uint32_t serial;
  LGMP_STATUS status = lgmpClientSendData(this->pointerQueue, buffer, size,
      &serial);
  LG_UNLOCK(this->pointerLock);
  if (status != LGMP_OK)
    return status == LGMP_ERR_INVALID_SESSION ? LG_TRANSPORT_DISCONNECTED :
      LG_TRANSPORT_ERROR;
  *token = serial;
  return LG_TRANSPORT_OK;
}

static LG_TransportStatus lgmp_controlStatus(LG_Transport * this,
    LG_TransportControlToken token)
{
  LG_LOCK(this->pointerLock);
  if (!this->pointerQueue)
  {
    LG_UNLOCK(this->pointerLock);
    return LG_TRANSPORT_DISCONNECTED;
  }
  uint32_t serial;
  LGMP_STATUS status = lgmpClientGetSerial(this->pointerQueue, &serial);
  LG_UNLOCK(this->pointerLock);
  if (status != LGMP_OK)
    return status == LGMP_ERR_INVALID_SESSION ? LG_TRANSPORT_DISCONNECTED :
      LG_TRANSPORT_ERROR;
  return (int32_t)(serial - (uint32_t)token) >= 0 ?
    LG_TRANSPORT_OK : LG_TRANSPORT_UNAVAILABLE;
}

const LG_TransportOps LGT_LGMP =
{
  .name           = "lgmp",
  .setup          = lgmp_setup,
  .create         = lgmp_create,
  .destroy        = lgmp_destroy,
  .connect        = lgmp_connect,
  .disconnect     = lgmp_disconnect,
  .sessionValid   = lgmp_sessionValid,
  .supportsDMA    = lgmp_supportsDMA,
  .attachRenderer = lgmp_attachRenderer,
  .detachRenderer = lgmp_detachRenderer,
  .nextFrame      = lgmp_nextFrame,
  .getFrameTiming = lgmp_getFrameTiming,
  .releaseFrame   = lgmp_releaseFrame,
  .stopFrame      = lgmp_stopFrame,
  .nextPointer    = lgmp_nextPointer,
  .releasePointer = lgmp_releasePointer,
  .stopPointer    = lgmp_stopPointer,
  .sendControl    = lgmp_sendControl,
  .controlStatus  = lgmp_controlStatus,
};
