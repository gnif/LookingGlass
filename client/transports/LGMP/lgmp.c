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

struct DMAFrameInfo
{
  const KVMFRFrame * frame;
  size_t             dataSize;
  int                fd;
};

struct LG_Transport
{
  struct IVSHMEM  shm;
  PLGMPClient     client;
  PLGMPClientQueue frameQueue;
  PLGMPClientQueue pointerQueue;
  LG_Lock         pointerLock;

  unsigned cursorPollInterval;
  unsigned framePollInterval;
  bool     allowDMA;
  bool     connected;
  bool     framePending;
  uint32_t frameSerial;
  bool     formatValid;
  LG_TransportFrameFormat format;

  struct DMAFrameInfo dma[LGMP_Q_FRAME_LEN];
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

  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    this->dma[i].fd = -1;

  LG_LOCK_INIT(this->pointerLock);

  if (!ivshmemOpenDev(&this->shm,
        option_get_string("lgmp", "shmDevice")))
  {
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
    LG_LOCK_FREE(this->pointerLock);
    free(this);
    return false;
  }

  *result = this;
  return true;
}

static void lgmp_stopFrame(struct LG_Transport * this)
{
  if (this->framePending && this->frameQueue)
  {
    const LGMP_STATUS status = lgmpClientMessageDone(this->frameQueue);
    if (status != LGMP_OK)
      DEBUG_WARN("Failed to release pending LGMP frame: %s",
          lgmpStatusString(status));
  }
  this->framePending = false;
  LGMP_STATUS status = lgmpClientUnsubscribe(&this->frameQueue);
  if (status != LGMP_OK)
  {
    DEBUG_WARN("Failed to unsubscribe from the LGMP frame queue: %s",
        lgmpStatusString(status));
    this->frameQueue = NULL;
  }

  this->frameSerial = 0;
  this->formatValid = false;
}

static void lgmp_stopPointer(struct LG_Transport * this)
{
  LG_LOCK(this->pointerLock);
  const LGMP_STATUS status = lgmpClientUnsubscribe(&this->pointerQueue);
  if (status != LGMP_OK)
  {
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
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
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
  LGMP_STATUS status = lgmpClientSessionInit(this->client, &size, &data, NULL,
      &remoteVersion);
  session->remoteVersion = remoteVersion;
  switch (status)
  {
    case LGMP_OK:
      if (!lgmp_parseSession(data, size, session))
        return LG_TRANSPORT_INVALID_VERSION;
      this->connected   = true;
      this->frameSerial = 0;
      this->formatValid = false;
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
  lgmp_closeDMA(this);
  this->connected = false;
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

static LG_TransportStatus lgmp_process(PLGMPClientQueue queue,
    unsigned interval, LGMPMessage * message)
{
  LGMP_STATUS status = lgmpClientProcess(queue, message);
  switch (status)
  {
    case LGMP_OK:
      return LG_TRANSPORT_OK;
    case LGMP_ERR_QUEUE_EMPTY:
      if (interval)
        usleep(interval);
      return LG_TRANSPORT_TIMEOUT;
    case LGMP_ERR_INVALID_SESSION:
      return LG_TRANSPORT_DISCONNECTED;
    default:
      DEBUG_ERROR("lgmpClientProcess failed: %s", lgmpStatusString(status));
      return LG_TRANSPORT_ERROR;
  }
}

static int lgmp_getDMA(struct LG_Transport * this, const KVMFRFrame * frame,
    size_t dataSize)
{
  struct DMAFrameInfo * dma = NULL;
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
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
    for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
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

static LG_TransportStatus lgmp_nextFrame(LG_Transport * this, bool useDMA,
    LG_TransportFrame * result)
{
  if (!this->connected)
    return LG_TRANSPORT_DISCONNECTED;
  if (this->framePending)
    return LG_TRANSPORT_ERROR;

  LG_TransportStatus status = lgmp_subscribe(this->client, LGMP_Q_FRAME,
      &this->frameQueue);
  if (status != LG_TRANSPORT_OK)
  {
    if (status == LG_TRANSPORT_TIMEOUT)
      usleep(1000);
    return status;
  }

  LGMPMessage message;
  status = lgmp_process(this->frameQueue, this->framePollInterval, &message);
  if (status != LG_TRANSPORT_OK)
    return status;

  if (message.size < sizeof(KVMFRFrame))
  {
    lgmpClientMessageDone(this->frameQueue);
    DEBUG_ERROR("LGMP frame payload is too small");
    return LG_TRANSPORT_ERROR;
  }

  const KVMFRFrame * frame = (const KVMFRFrame *)message.mem;
  const size_t frameDataSize = (size_t)frame->dataHeight * frame->pitch;
  if (frame->type <= FRAME_TYPE_INVALID || frame->type >= FRAME_TYPE_MAX ||
      frame->offset > message.size - sizeof(FrameBuffer) ||
      frameDataSize > message.size - frame->offset - sizeof(FrameBuffer))
  {
    lgmpClientMessageDone(this->frameQueue);
    DEBUG_ERROR("LGMP frame payload contains invalid dimensions or offsets");
    return LG_TRANSPORT_ERROR;
  }

  if (frame->frameSerial == this->frameSerial && this->frameSerial)
  {
    lgmpClientMessageDone(this->frameQueue);
    return LG_TRANSPORT_TIMEOUT;
  }
  this->frameSerial = frame->frameSerial;

  memset(result, 0, sizeof(*result));
  result->serial = frame->frameSerial;
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
  result->format = format;

  result->framebuffer = (const FrameBuffer *)((const uint8_t *)frame +
      frame->offset);
  result->dmaFD       = -1;
  if (useDMA)
  {
    const size_t dataSize = (size_t)format->dataHeight * format->pitch;
    result->dmaFD = lgmp_getDMA(this, frame, dataSize);
    if (result->dmaFD < 0)
    {
      lgmpClientMessageDone(this->frameQueue);
      DEBUG_ERROR("Failed to obtain a DMA buffer for the frame");
      return LG_TRANSPORT_ERROR;
    }
  }

  if (frame->damageRectsCount <= KVMFR_MAX_DAMAGE_RECTS)
  {
    result->damageRects      = frame->damageRects;
    result->damageRectsCount = frame->damageRectsCount;
  }
  else
    DEBUG_WARN("Invalid damage rectangles, forcing a full update");

  this->framePending = true;
  return LG_TRANSPORT_OK;
}

static void lgmp_releaseFrame(LG_Transport * this, LG_TransportFrame * frame)
{
  if (this->framePending && this->frameQueue)
    lgmpClientMessageDone(this->frameQueue);
  this->framePending = false;
  memset(frame, 0, sizeof(*frame));
}

static LG_TransportStatus lgmp_nextPointer(LG_Transport * this,
    LG_TransportPointer * result)
{
  if (!this->connected)
    return LG_TRANSPORT_DISCONNECTED;

  uint32_t pointerFlags  = 0;
  size_t   shapeSize     = 0;
  size_t   transformSize = 0;
  LG_TransportStatus status = LG_TRANSPORT_OK;
  if (!this->pointerQueue)
  {
    LG_LOCK(this->pointerLock);
    status = lgmp_subscribe(this->client, LGMP_Q_POINTER,
        &this->pointerQueue);
    LG_UNLOCK(this->pointerLock);
    if (status == LG_TRANSPORT_TIMEOUT)
      usleep(1000);
  }
  if (status == LG_TRANSPORT_OK)
  {
    LGMPMessage message;
    status = lgmp_process(this->pointerQueue, this->cursorPollInterval,
        &message);
    if (status == LG_TRANSPORT_OK)
    {
      if (message.size < sizeof(KVMFRCursor))
      {
        lgmpClientMessageDone(this->pointerQueue);
        status = LG_TRANSPORT_ERROR;
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
          lgmpClientMessageDone(this->pointerQueue);
          status = LG_TRANSPORT_ERROR;
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
            pointerFlags = message.udata;
          }
          lgmpClientMessageDone(this->pointerQueue);
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
  uint8_t buffer[sizeof(KVMFRWindowSize)];
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
  return serial >= token ? LG_TRANSPORT_OK : LG_TRANSPORT_UNAVAILABLE;
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
  .releaseFrame   = lgmp_releaseFrame,
  .stopFrame      = lgmp_stopFrame,
  .nextPointer    = lgmp_nextPointer,
  .releasePointer = lgmp_releasePointer,
  .stopPointer    = lgmp_stopPointer,
  .sendControl    = lgmp_sendControl,
  .controlStatus  = lgmp_controlStatus,
};
