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

#include "clipboard.h"
#include "input.h"

#include "common/KVMFR.h"
#include "common/KVMFRRecovery.h"
#include "common/LGMPConfig.h"
#include "common/debug.h"
#include "common/event.h"
#include "common/ivshmem.h"
#include "common/locking.h"
#include "common/option.h"
#include "common/stringutils.h"
#include "common/time.h"

#include <lgmp/client.h>

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LGMP_TIMING_SPIN_COUNT 4096
#define LGMP_RECOVERY_PROBE_INTERVAL_US 10000U
#define LGMP_RECOVERY_PROBE_TIMEOUT_US \
  ((KVMFR_R_HEARTBEAT_MS * 3U) * 1000U)
#define LGMP_RECOVERY_LIVE_TIMEOUT_US \
  ((KVMFR_R_HEARTBEAT_MS * 4U) * 1000U)

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
  uint64_t                  receiveTime;
  uint64_t                  prepareTime;
  bool                      providerValid;
  bool                      releaseRequested;
  bool                      active;
};

typedef struct LGMPVideoStatusUpdate
{
  bool frame;
  bool frameAvailable;
  bool frameReplaced;
  LG_TransportStatus frameReason;
  bool pointer;
  bool pointerAvailable;
  bool pointerReplaced;
  LG_TransportStatus pointerReason;
}
LGMPVideoStatusUpdate;

typedef struct LGMPVideoStatusDispatch
{
  LG_VideoStatusFn callback;
  void *           opaque;
  uint64_t         listenerSerial;
  bool             pending;
}
LGMPVideoStatusDispatch;

typedef struct LGMPVideoCallbackContext
{
  struct LGMPVideoCallbackContext * previous;
  LG_Transport                    * transport;
}
LGMPVideoCallbackContext;

struct LG_Transport
{
  struct IVSHMEM    shm;
  PLGMPClient       client;
  PLGMPClientQueue  frameQueue;
  PLGMPClientQueue  ownerFrameQueue[LGMP_Q_FRAME_LEN];
  PLGMPClientQueue  pointerQueue;
  LGMPInput        * input;
  LGMPClipboard    * clipboard;
  LG_Lock           frameLock;
  LG_Lock           pointerLock;
  LG_RWLock         videoStatusLock;
  LGEvent         * frameWake;
  LGEvent         * pointerWake;

  LG_VideoStatusFn videoStatusCallback;
  void           * videoStatusOpaque;
  atomic_uint      videoStatusCallbacks;
  uint64_t         videoStatusListenerSerial;
  uint64_t         videoFrameEpoch;
  uint64_t         videoPointerEpoch;
  LG_TransportStatus videoFrameReason;
  LG_TransportStatus videoPointerReason;
  bool             videoFrameAvailable;
  bool             videoPointerAvailable;

  unsigned cursorPollInterval;
  unsigned framePollInterval;
  bool     allowDMA;
  atomic_bool connected;
  bool     frameStopRequested;
  bool     frameScheduleSupported;
  atomic_bool inputSupported;
  atomic_bool clipboardSupported;
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

  size_t      lgmpSize;
  KVMFRR    * recovery;
  LG_Lock     recoveryLock;
  uint64_t    recoveryCandidateSession;
  uint64_t    recoverySession;
  uint64_t    recoveryHeartbeatTime;
  uint32_t    recoveryCandidateHeartbeat;
  uint32_t    recoveryLGMPVersion;
  bool        recoveryLive;
  atomic_bool destroyPending;
};

static LG_Lock l_videoStatusCallbackLock = ATOMIC_FLAG_INIT;
static _Thread_local LGMPVideoCallbackContext * l_videoStatusContext;

#ifdef ENABLE_TESTS
static atomic_bool * l_videoStatusDispatchWaiting;
static atomic_bool * l_videoStatusDispatchHold;
static LG_Transport * l_videoStatusDispatchTarget;
static LG_Lock        l_videoStatusTestLock = ATOMIC_FLAG_INIT;

void lgmp_testSetVideoStatusDispatchGate(
    LG_Transport * target, atomic_bool * waiting, atomic_bool * hold)
{
  LG_LOCK(l_videoStatusTestLock);
  l_videoStatusDispatchTarget  = target;
  l_videoStatusDispatchWaiting = waiting;
  l_videoStatusDispatchHold    = hold;
  LG_UNLOCK(l_videoStatusTestLock);
}
#endif

static void lgmp_setVideoStatusListener(LG_Transport * this,
    LG_VideoStatusFn callback, void * callbackOpaque);
static void lgmp_destroyNow(LG_Transport * this);
static void lgmp_releaseFrame(LG_Transport * this,
    LG_TransportFrame * frame);

static unsigned lgmp_videoStatusOwnCallbacks(const LG_Transport * transport)
{
  unsigned count = 0;
  for (const LGMPVideoCallbackContext * context = l_videoStatusContext;
       context; context = context->previous)
    count += context->transport == transport;
  return count;
}

static bool lgmp_videoStatusContextContains(const LG_Transport * transport)
{
  for (const LGMPVideoCallbackContext * context = l_videoStatusContext;
       context; context = context->previous)
    if (context->transport == transport)
      return true;
  return false;
}

static void lgmp_waitVideoStatusCallbacks(
    LG_Transport * transport, unsigned ownCallbacks)
{
  while (atomic_load_explicit(
        &transport->videoStatusCallbacks, memory_order_acquire) >
      ownCallbacks)
    nsleep(1000000U);

  /* A retiring callback publishes the count while holding this lock.  Taking
   * it here ensures that callback has completed every access to the instance
   * before an external caller tears it down. */
  LG_LOCK_EXCLUSIVE(transport->videoStatusLock);
  LG_UNLOCK_EXCLUSIVE(transport->videoStatusLock);
}

static bool lgmp_releaseVideoStatusCallback(LG_Transport * transport)
{
  /* Linearize the final reference and a nested destroy request.  Whichever
   * side acquires the lock first either observes the pending request or leaves
   * a zero count for the destroy path to observe. */
  LG_LOCK_EXCLUSIVE(transport->videoStatusLock);
  const bool destroy = atomic_load_explicit(
      &transport->destroyPending, memory_order_acquire);
  const unsigned previous = atomic_fetch_sub_explicit(
      &transport->videoStatusCallbacks, 1, memory_order_acq_rel);
  DEBUG_ASSERT(previous != 0);
  LG_UNLOCK_EXCLUSIVE(transport->videoStatusLock);
  return destroy && previous == 1;
}

static void lgmp_retainVideoStatusLifetime(LG_Transport * transport)
{
  atomic_fetch_add_explicit(
      &transport->videoStatusCallbacks, 1, memory_order_acq_rel);
}

static bool lgmp_destroyRequested(const LG_Transport * transport)
{
  return atomic_load_explicit(
      &transport->destroyPending, memory_order_acquire);
}

static void lgmp_releaseVideoStatusLifetime(LG_Transport * transport)
{
  if (lgmp_releaseVideoStatusCallback(transport))
    lgmp_destroyNow(transport);
}

static uint64_t lgmp_nextEpoch(uint64_t epoch)
{
  if (++epoch == 0)
    ++epoch;
  return epoch;
}

static void lgmp_callVideoStatusCallback(LG_VideoStatusFn callback,
    void * opaque,
    LG_Transport * transport,
    const LG_VideoStatus * status)
{
  LGMPVideoCallbackContext context =
  {
    .previous  = l_videoStatusContext,
    .transport = transport,
  };
  l_videoStatusContext = &context;
  callback(opaque, status);
  l_videoStatusContext = context.previous;
}

static LG_VideoStatus lgmp_videoStatusLocked(const LG_Transport * this)
{
  return (LG_VideoStatus)
  {
    .frame =
    {
      .available  = this->videoFrameAvailable,
      .epoch      = this->videoFrameEpoch,
      .reason     = this->videoFrameAvailable ? LG_TRANSPORT_OK :
        this->videoFrameReason,
    },
    .pointer =
    {
      .available  = this->videoPointerAvailable,
      .epoch      = this->videoPointerEpoch,
      .reason     = this->videoPointerAvailable ? LG_TRANSPORT_OK :
        this->videoPointerReason,
    },
  };
}

static LG_VideoStatus lgmp_updateVideoStatus(LG_Transport * this,
    const LGMPVideoStatusUpdate update,
    LGMPVideoStatusDispatch * dispatch)
{
  *dispatch = (LGMPVideoStatusDispatch) { 0 };
  LG_VideoStatus published;

  LG_LOCK_EXCLUSIVE(this->videoStatusLock);
  const LG_TransportStatus frameReason = update.frameAvailable ?
    LG_TRANSPORT_OK : update.frameReason == LG_TRANSPORT_OK ?
    LG_TRANSPORT_UNAVAILABLE : update.frameReason;
  const LG_TransportStatus pointerReason = update.pointerAvailable ?
    LG_TRANSPORT_OK : update.pointerReason == LG_TRANSPORT_OK ?
    LG_TRANSPORT_UNAVAILABLE : update.pointerReason;
  const bool frameChanged = update.frame &&
    this->videoFrameAvailable != update.frameAvailable;
  const bool pointerChanged = update.pointer &&
    this->videoPointerAvailable != update.pointerAvailable;
  const bool frameReasonChanged = update.frame &&
    this->videoFrameReason != frameReason;
  const bool pointerReasonChanged = update.pointer &&
    this->videoPointerReason != pointerReason;
  if (frameChanged || update.frameReplaced)
    this->videoFrameEpoch = lgmp_nextEpoch(this->videoFrameEpoch);
  if (pointerChanged || update.pointerReplaced)
    this->videoPointerEpoch = lgmp_nextEpoch(this->videoPointerEpoch);
  if (update.frame)
  {
    this->videoFrameAvailable = update.frameAvailable;
    this->videoFrameReason    = frameReason;
  }
  if (update.pointer)
  {
    this->videoPointerAvailable = update.pointerAvailable;
    this->videoPointerReason    = pointerReason;
  }
  published = lgmp_videoStatusLocked(this);

  dispatch->callback       = this->videoStatusCallback;
  dispatch->opaque         = this->videoStatusOpaque;
  dispatch->listenerSerial = this->videoStatusListenerSerial;
  if (!dispatch->callback || (!frameChanged && !pointerChanged &&
        !frameReasonChanged && !pointerReasonChanged &&
        !update.frameReplaced && !update.pointerReplaced))
    dispatch->callback = NULL;
  if (dispatch->callback)
    atomic_fetch_add_explicit(
        &this->videoStatusCallbacks, 1, memory_order_acq_rel);
  dispatch->pending = dispatch->callback != NULL;
  LG_UNLOCK_EXCLUSIVE(this->videoStatusLock);
  return published;
}

static void lgmp_dispatchVideoStatus(LG_Transport * this,
    const LGMPVideoStatusDispatch * dispatch)
{
  if (dispatch->pending)
  {
    const bool serialized = l_videoStatusContext == NULL;
    if (serialized)
    {
#ifdef ENABLE_TESTS
      LG_LOCK(l_videoStatusTestLock);
      LG_Transport * target = l_videoStatusDispatchTarget;
      atomic_bool * waiting = l_videoStatusDispatchWaiting;
      atomic_bool * hold    = l_videoStatusDispatchHold;
      LG_UNLOCK(l_videoStatusTestLock);
      if ((!target || target == this) && waiting)
        atomic_store_explicit(
            waiting, true, memory_order_release);
      if ((!target || target == this) && hold)
        while (atomic_load_explicit(
            hold, memory_order_acquire))
          nsleep(1000000U);
#endif
      LG_LOCK(l_videoStatusCallbackLock);
    }
    LG_LOCK_SHARED(this->videoStatusLock);
    const bool current =
      this->videoStatusCallback == dispatch->callback &&
      this->videoStatusOpaque == dispatch->opaque &&
      this->videoStatusListenerSerial == dispatch->listenerSerial;
    const LG_VideoStatus status = lgmp_videoStatusLocked(this);
    LG_UNLOCK_SHARED(this->videoStatusLock);
    if (current)
      lgmp_callVideoStatusCallback(
          dispatch->callback, dispatch->opaque, this, &status);
    const bool destroy = lgmp_releaseVideoStatusCallback(this);
    if (serialized)
      LG_UNLOCK(l_videoStatusCallbackLock);
    if (destroy)
      lgmp_destroyNow(this);
  }
}

static LG_VideoStatus lgmp_publishVideoStatus(LG_Transport * this,
    const LGMPVideoStatusUpdate update)
{
  LGMPVideoStatusDispatch dispatch;
  const LG_VideoStatus published =
    lgmp_updateVideoStatus(this, update, &dispatch);
  lgmp_dispatchVideoStatus(this, &dispatch);
  return published;
}

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

static bool lgmp_recoveryMagic(const KVMFRR * recovery)
{
  return recovery && memcmp(recovery->header.magic, KVMFR_R_MAGIC,
      sizeof(recovery->header.magic)) == 0;
}

static bool lgmp_recoverySnapshot(const struct LG_Transport * this,
    KVMFRRHeader * header, KVMFRRInfo * info)
{
  const KVMFRR * recovery = this->recovery;
  if (!recovery ||
      __atomic_load_n(&recovery->header.ready, __ATOMIC_ACQUIRE) !=
        KVMFR_R_READY)
    return false;

  memcpy(header, &recovery->header, sizeof(*header));
  memcpy(info, &recovery->info, sizeof(*info));
  header->heartbeat = __atomic_load_n(&recovery->header.heartbeat,
      __ATOMIC_ACQUIRE);

  if (__atomic_load_n(&recovery->header.ready, __ATOMIC_ACQUIRE) !=
        KVMFR_R_READY ||
      memcmp(header->magic, KVMFR_R_MAGIC,
        sizeof(header->magic)) != 0 ||
      header->abiVersion != KVMFR_R_VERSION ||
      header->structSize < sizeof(KVMFRR) ||
      header->session == 0)
    return false;

  return true;
}

static bool lgmp_cancelled(LG_TransportCancelledFn cancelled, void * opaque)
{
  return cancelled && cancelled(opaque);
}

static bool lgmp_refreshRecoveryLockedCancellable(
    struct LG_Transport * this, bool wait,
    LG_TransportCancelledFn cancelled, void * opaque)
{
  const uint64_t deadline = wait ?
    microtime() + LGMP_RECOVERY_PROBE_TIMEOUT_US : 0;

  do
  {
    if (lgmp_cancelled(cancelled, opaque))
      break;

    KVMFRRHeader header;
    KVMFRRInfo info;
    const bool valid = lgmp_recoverySnapshot(this, &header, &info);
    const uint64_t now = microtime();

    if (valid)
    {
      if (this->recoveryCandidateSession != header.session)
      {
        this->recoveryCandidateSession   = header.session;
        this->recoveryCandidateHeartbeat = header.heartbeat;
        this->recoveryLive               = false;
        this->lgmpSize                   = this->shm.size;
      }
      else if (this->recoveryCandidateHeartbeat != header.heartbeat)
      {
        this->recoveryCandidateHeartbeat = header.heartbeat;
        this->recoverySession            = header.session;
        this->recoveryHeartbeatTime      = now;
        this->recoveryLive               = true;
        this->lgmpSize = this->shm.size - KVMFR_R_REGION_SIZE;
      }

      if (this->recoveryLive && this->recoverySession == header.session &&
          now - this->recoveryHeartbeatTime <=
            LGMP_RECOVERY_LIVE_TIMEOUT_US)
      {
        this->recoveryLGMPVersion = header.lgmpVersion;
        return true;
      }
    }
    else
    {
      this->recoveryLive = false;
      this->lgmpSize     = this->shm.size;
    }

    if (!wait || now >= deadline)
      break;
    usleep(LGMP_RECOVERY_PROBE_INTERVAL_US);
  }
  while (true);

  this->recoveryLive = false;
  this->lgmpSize     = this->shm.size;
  return false;
}

static bool lgmp_refreshRecoveryLocked(struct LG_Transport * this, bool wait)
{
  return lgmp_refreshRecoveryLockedCancellable(
      this, wait, NULL, NULL);
}

static bool lgmp_recoveryVersions(struct LG_Transport * this,
    uint32_t * lgmpVersion, uint32_t * kvmfrVersion)
{
  LG_LOCK(this->recoveryLock);
  const bool live = lgmp_refreshRecoveryLocked(this,
      lgmp_recoveryMagic(this->recovery));
  KVMFRRHeader header;
  KVMFRRInfo info;
  const bool valid = live && lgmp_recoverySnapshot(this, &header, &info) &&
    header.session == this->recoverySession;
  if (valid)
  {
    if (lgmpVersion)
      *lgmpVersion = header.lgmpVersion;
    if (kvmfrVersion)
      *kvmfrVersion = header.kvmfrVersion;
  }
  LG_UNLOCK(this->recoveryLock);
  return valid;
}

static LGMP_STATUS lgmp_initializeClient(struct LG_Transport * this)
{
  LGMP_STATUS status = lgmpClientInit(this->shm.mem, this->lgmpSize,
      &this->client);
  if (status != LGMP_OK)
    return status;

  if (!lgmpInput_create(this->client, &this->input))
  {
    lgmpClientFree(&this->client);
    return LGMP_ERR_NO_MEM;
  }

  if (!lgmpClipboard_create(this->client, &this->clipboard))
  {
    lgmpInput_destroy(&this->input);
    lgmpClientFree(&this->client);
    return LGMP_ERR_NO_MEM;
  }

  return LGMP_OK;
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
  atomic_init(&this->connected, false);
  atomic_init(&this->inputSupported, false);
  atomic_init(&this->clipboardSupported, false);
  atomic_init(&this->destroyPending, false);

  this->frameWake   = lgCreateEvent(true, 0);
  this->pointerWake = lgCreateEvent(true, 0);
  if (!this->frameWake || !this->pointerWake)
  {
    if (this->frameWake)
      lgFreeEvent(this->frameWake);
    if (this->pointerWake)
      lgFreeEvent(this->pointerWake);
    free(this);
    return false;
  }

  for (unsigned i = 0; i < LGMP_Q_FRAME_BUFFER_LEN; ++i)
    this->dma[i].fd = -1;

  LG_LOCK_INIT(this->frameLock);
  LG_LOCK_INIT(this->pointerLock);
  LG_RWLOCK_INIT(this->videoStatusLock);
  LG_LOCK_INIT(this->recoveryLock);
  atomic_init(&this->videoStatusCallbacks, 0);

  this->frameGeneration = 1;
  this->frameLease[0].subscription = &this->frameQueue;
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    this->frameLease[i + 1].subscription = &this->ownerFrameQueue[i];

  if (!ivshmemOpenDev(&this->shm,
        option_get_string("lgmp", "shmDevice")))
  {
    LG_LOCK_FREE(this->frameLock);
    LG_LOCK_FREE(this->pointerLock);
    LG_RWLOCK_FREE(this->videoStatusLock);
    LG_LOCK_FREE(this->recoveryLock);
    lgFreeEvent(this->frameWake);
    lgFreeEvent(this->pointerWake);
    free(this);
    return false;
  }

  this->lgmpSize = this->shm.size;
  if (this->shm.size >= KVMFR_R_REGION_SIZE + sizeof(KVMFRR))
  {
    this->recovery = (KVMFRR *)((uint8_t *)this->shm.mem +
        this->shm.size - KVMFR_R_REGION_SIZE);
    if (lgmp_recoveryMagic(this->recovery))
    {
      LG_LOCK(this->recoveryLock);
      lgmp_refreshRecoveryLocked(this, true);
      LG_UNLOCK(this->recoveryLock);
    }
  }

  LGMP_STATUS status = lgmp_initializeClient(this);
  if (status != LGMP_OK)
  {
    if (this->recoveryLive &&
        (status == LGMP_ERR_INVALID_MAGIC ||
         status == LGMP_ERR_INVALID_VERSION))
    {
      DEBUG_WARN("LGMP is unavailable (%s), recovery remains available",
          lgmpStatusString(status));
      lgmpClientFree(&this->client);
      *result = this;
      return true;
    }

    DEBUG_ERROR("lgmpClientInit failed: %s", lgmpStatusString(status));
    ivshmemClose(&this->shm);
    LG_LOCK_FREE(this->frameLock);
    LG_LOCK_FREE(this->pointerLock);
    LG_RWLOCK_FREE(this->videoStatusLock);
    LG_LOCK_FREE(this->recoveryLock);
    lgFreeEvent(this->frameWake);
    lgFreeEvent(this->pointerWake);
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
  lgmp_publishVideoStatus(this, (LGMPVideoStatusUpdate)
  {
    .frame         = true,
    .frameReason   = LG_TRANSPORT_DISCONNECTED,
    .pointer       = true,
    .pointerReason = LG_TRANSPORT_DISCONNECTED,
  });
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

static void lgmp_destroyNow(LG_Transport * this)
{
  if (this->client)
  {
    lgmpClipboard_destroy(&this->clipboard);
    lgmpInput_destroy(&this->input);
    lgmp_closeQueues(this);
    lgmpClientFree(&this->client);
  }
  lgmp_closeDMA(this);
  free(this->pointerData);
  ivshmemClose(&this->shm);
  LG_LOCK_FREE(this->frameLock);
  LG_LOCK_FREE(this->pointerLock);
  LG_RWLOCK_FREE(this->videoStatusLock);
  LG_LOCK_FREE(this->recoveryLock);
  lgFreeEvent(this->frameWake);
  lgFreeEvent(this->pointerWake);
  free(this);
}

static void lgmp_destroy(LG_Transport ** transport)
{
  if (!transport || !*transport)
    return;

  struct LG_Transport * this = *transport;
  *transport = NULL;
  const bool nested = l_videoStatusContext != NULL;
  lgmp_setVideoStatusListener(this, NULL, NULL);
  if (nested)
  {
    LG_LOCK_EXCLUSIVE(this->videoStatusLock);
    atomic_store_explicit(
        &this->destroyPending, true, memory_order_release);
    const bool deferred = lgmp_videoStatusContextContains(this) ||
      atomic_load_explicit(
          &this->videoStatusCallbacks, memory_order_acquire) != 0;
    LG_UNLOCK_EXCLUSIVE(this->videoStatusLock);
    if (deferred)
      return;
  }

  lgmp_destroyNow(this);
}

static void lgmp_setVersionMismatch(LG_TransportSession * session,
    const char * component, uint32_t expected, uint32_t current)
{
  session->versionMismatch.valid           = true;
  session->versionMismatch.expectedVersion = expected;
  session->versionMismatch.currentVersion  = current;
  str_copy(session->versionMismatch.component,
      sizeof(session->versionMismatch.component), component,
      strlen(component));
}

static bool lgmp_parseSession(struct LG_Transport * this,
    const uint8_t * data, uint32_t size, LG_TransportSession * session)
{
  if (!data || size < sizeof(KVMFR))
  {
    uint32_t current = 0;
    bool currentValid = data &&
      size >= offsetof(KVMFR, version) + sizeof(uint32_t);
    if (currentValid)
    {
      memcpy(&current, data + offsetof(KVMFR, version), sizeof(current));
    }
    else
      currentValid = lgmp_recoveryVersions(this, NULL, &current);
    if (currentValid && current != KVMFR_VERSION)
      lgmp_setVersionMismatch(session, "KVMFR", KVMFR_VERSION, current);
    return false;
  }

  const KVMFR * header = (const KVMFR *)data;
  if (memcmp(header->magic, KVMFR_MAGIC, sizeof(header->magic)) != 0)
  {
    uint32_t current;
    if (lgmp_recoveryVersions(this, NULL, &current) &&
        current != KVMFR_VERSION)
      lgmp_setVersionMismatch(session, "KVMFR", KVMFR_VERSION, current);
    return false;
  }

  if (header->version != KVMFR_VERSION)
  {
    lgmp_setVersionMismatch(session, "KVMFR", KVMFR_VERSION,
        header->version);
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
  if (header->features & KVMFR_FEATURE_INPUT)
    session->features |= LG_TRANSPORT_FEATURE_INPUT;
  if (header->features & KVMFR_FEATURE_CLIPBOARD)
    session->features |= LG_TRANSPORT_FEATURE_CLIPBOARD;

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

static LG_TransportStatus lgmp_connectInternal(LG_Transport * this,
    LG_TransportSession * session, LG_TransportCancelledFn cancelled,
    void * opaque)
{
  memset(session, 0, sizeof(*session));
  session->os = LG_TRANSPORT_OS_OTHER;

  if (lgmp_cancelled(cancelled, opaque))
    return LG_TRANSPORT_DISCONNECTED;

  if (!this->client)
  {
    LG_LOCK(this->recoveryLock);
    lgmp_refreshRecoveryLockedCancellable(this,
        lgmp_recoveryMagic(this->recovery), cancelled, opaque);
    LG_UNLOCK(this->recoveryLock);

    if (lgmp_cancelled(cancelled, opaque))
      return LG_TRANSPORT_DISCONNECTED;

    const LGMP_STATUS status = lgmp_initializeClient(this);
    if (status != LGMP_OK)
    {
      uint32_t remoteVersion = 0;
      const bool versionKnown =
        lgmp_recoveryVersions(this, &remoteVersion, NULL);
      if (status == LGMP_ERR_INVALID_VERSION ||
          (status == LGMP_ERR_INVALID_MAGIC && versionKnown &&
           remoteVersion != LGMP_PROTOCOL_VERSION))
      {
        if (versionKnown && remoteVersion != LGMP_PROTOCOL_VERSION)
          lgmp_setVersionMismatch(session, "LGMP",
              LGMP_PROTOCOL_VERSION, remoteVersion);
        return LG_TRANSPORT_INVALID_VERSION;
      }

      return status == LGMP_ERR_INVALID_MAGIC ?
        LG_TRANSPORT_UNAVAILABLE : LG_TRANSPORT_ERROR;
    }
  }

  if (lgmp_cancelled(cancelled, opaque))
    return LG_TRANSPORT_DISCONNECTED;

  uint32_t size;
  uint8_t * data;
  uint32_t remoteVersion = 0;
  LGMP_STATUS status = lgmpClientSessionInit(this->client, &size, &data,
      &this->clientID, &remoteVersion);
  switch (status)
  {
    case LGMP_OK:
      if (!lgmp_parseSession(this, data, size, session))
        return LG_TRANSPORT_INVALID_VERSION;

      LGMPVideoStatusDispatch dispatch;
      LG_LOCK(this->frameLock);
      if (++this->frameGeneration == 0)
        ++this->frameGeneration;
      this->frameStopRequested     = false;
      this->frameScheduleSupported =
        session->features & LG_TRANSPORT_FEATURE_FRAME_SCHEDULE;
      atomic_store_explicit(&this->inputSupported,
          session->features & LG_TRANSPORT_FEATURE_INPUT,
          memory_order_release);
      atomic_store_explicit(&this->clipboardSupported,
          session->features & LG_TRANSPORT_FEATURE_CLIPBOARD,
          memory_order_release);
      this->frameSerial            = 0;
      this->frameSerialValid       = false;
      this->formatValid            = false;
      lgmp_updateVideoStatus(this, (LGMPVideoStatusUpdate)
      {
        .frame   = true,
        .pointer = true,
      }, &dispatch);
      atomic_store_explicit(
          &this->connected, true, memory_order_release);
      LG_UNLOCK(this->frameLock);
      lgmp_dispatchVideoStatus(this, &dispatch);
      return LG_TRANSPORT_OK;

    case LGMP_ERR_INVALID_VERSION:
      if (!remoteVersion)
        lgmp_recoveryVersions(this, &remoteVersion, NULL);
      lgmp_setVersionMismatch(session, "LGMP", LGMP_PROTOCOL_VERSION,
          remoteVersion);
      return LG_TRANSPORT_INVALID_VERSION;

    case LGMP_ERR_INVALID_SESSION:
      return LG_TRANSPORT_UNAVAILABLE;

    case LGMP_ERR_INVALID_MAGIC:
      if (lgmp_recoveryVersions(this, &remoteVersion, NULL) &&
          remoteVersion != LGMP_PROTOCOL_VERSION)
      {
        lgmp_setVersionMismatch(session, "LGMP", LGMP_PROTOCOL_VERSION,
            remoteVersion);
        return LG_TRANSPORT_INVALID_VERSION;
      }
      return LG_TRANSPORT_UNAVAILABLE;

    default:
      DEBUG_ERROR("lgmpClientSessionInit failed: %s", lgmpStatusString(status));
      return LG_TRANSPORT_ERROR;
  }
}

static LG_TransportStatus lgmp_connect(LG_Transport * this,
    LG_TransportSession * session)
{
  lgmp_retainVideoStatusLifetime(this);
  LG_TransportStatus status =
    lgmp_connectInternal(this, session, NULL, NULL);
  if (lgmp_destroyRequested(this))
    status = LG_TRANSPORT_DISCONNECTED;
  lgmp_releaseVideoStatusLifetime(this);
  return status;
}

static void lgmp_disconnect(LG_Transport * this)
{
  if (!this->client)
    return;

  lgmp_retainVideoStatusLifetime(this);

  lgmpInput_disconnect(this->input);
  lgmpClipboard_disconnect(this->clipboard);

  LG_LOCK(this->frameLock);
  if (++this->frameGeneration == 0)
    ++this->frameGeneration;
  atomic_store_explicit(
      &this->connected, false, memory_order_release);
  this->frameScheduleSupported = false;
  atomic_store_explicit(
      &this->inputSupported, false, memory_order_release);
  atomic_store_explicit(
      &this->clipboardSupported, false, memory_order_release);
  this->clientID               = 0;
  LG_UNLOCK(this->frameLock);

  lgmp_closeQueues(this);
  lgmp_closeDMA(this);
  lgmp_releaseVideoStatusLifetime(this);
}

static LG_TransportStatus lgmp_connectCancellable(LG_Transport * this,
    LG_TransportSession * session, LG_TransportCancelledFn cancelled,
    void * opaque)
{
  lgmp_retainVideoStatusLifetime(this);
  const LG_TransportStatus status = lgmp_connectInternal(
      this, session, cancelled, opaque);
  LG_TransportStatus result = status;
  if (lgmp_destroyRequested(this))
    result = LG_TRANSPORT_DISCONNECTED;
  else if (lgmp_cancelled(cancelled, opaque))
  {
    if (status == LG_TRANSPORT_OK)
      lgmp_disconnect(this);
    result = LG_TRANSPORT_DISCONNECTED;
  }

  lgmp_releaseVideoStatusLifetime(this);
  return result;
}

static bool lgmp_sessionValid(LG_Transport * this)
{
  return this->client && atomic_load_explicit(
      &this->connected, memory_order_acquire) &&
    lgmpClientSessionValid(this->client);
}

static LG_RecoveryRequest lgmp_recoveryRequestType(uint32_t request)
{
  switch (request)
  {
    case KVMFR_R_REQ_NORMAL:
      return LG_RECOVERY_REQ_NORMAL;
    case KVMFR_R_REQ_RECOVERY:
      return LG_RECOVERY_REQ_RECOVERY;
    default:
      return LG_RECOVERY_REQ_NONE;
  }
}

static LG_RecoveryState lgmp_recoveryState(uint32_t state)
{
  switch (state)
  {
    case KVMFR_R_STATE_NORMAL:
      return LG_RECOVERY_STATE_NORMAL;
    case KVMFR_R_STATE_SWITCHING:
      return LG_RECOVERY_STATE_SWITCHING;
    case KVMFR_R_STATE_ACTIVE:
      return LG_RECOVERY_STATE_ACTIVE;
    case KVMFR_R_STATE_FAILED:
      return LG_RECOVERY_STATE_FAILED;
    default:
      return LG_RECOVERY_STATE_UNKNOWN;
  }
}

static LG_RecoveryError lgmp_recoveryError(uint32_t error)
{
  switch (error)
  {
    case KVMFR_R_ERR_NONE:
      return LG_RECOVERY_ERR_NONE;
    case KVMFR_R_ERR_UNSUPPORTED:
      return LG_RECOVERY_ERR_UNSUPPORTED;
    case KVMFR_R_ERR_HELPER_UNAVAILABLE:
      return LG_RECOVERY_ERR_HELPER_UNAVAILABLE;
    case KVMFR_R_ERR_TOPOLOGY_FAILED:
      return LG_RECOVERY_ERR_TOPOLOGY_FAILED;
    case KVMFR_R_ERR_NO_FALLBACK_DISPLAY:
      return LG_RECOVERY_ERR_NO_FALLBACK_DISPLAY;
    case KVMFR_R_ERR_BUSY:
      return LG_RECOVERY_ERR_BUSY;
    case KVMFR_R_ERR_CAPACITY:
      return LG_RECOVERY_ERR_CAPACITY;
    default:
      return LG_RECOVERY_ERR_UNSUPPORTED;
  }
}

#ifdef ENABLE_TESTS
LG_RecoveryError lgmp_testRecoveryError(uint32_t error)
{
  return lgmp_recoveryError(error);
}

bool lgmp_testRecoveryProbeCancellation(
    LG_TransportCancelledFn cancelled, void * opaque)
{
  struct LG_Transport transport =
  {
    .lgmpSize = 1,
  };
  LG_LOCK_INIT(transport.recoveryLock);
  const bool result = lgmp_refreshRecoveryLockedCancellable(
      &transport, true, cancelled, opaque);
  LG_LOCK_FREE(transport.recoveryLock);
  return result;
}
#endif

static bool lgmp_recoveryRequestSnapshot(const KVMFRRRequest * source,
    KVMFRRRequest * result)
{
  for (unsigned i = 0; i < 4; ++i)
  {
    const uint32_t serial = __atomic_load_n(&source->serial,
        __ATOMIC_ACQUIRE);
    if (serial & KVMFR_R_REQ_WRITING)
      continue;

    result->request = source->request;
    result->session = source->session;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&source->serial, __ATOMIC_RELAXED) == serial)
    {
      result->serial = serial;
      return true;
    }
  }

  return false;
}

static bool lgmp_recoveryStatusSnapshot(const KVMFRRStatus * source,
    KVMFRRStatus * result)
{
  for (unsigned i = 0; i < 4; ++i)
  {
    const uint32_t serial = __atomic_load_n(&source->serial,
        __ATOMIC_ACQUIRE);
    if (!serial || (serial & 1U))
      continue;

    result->ackSerial  = source->ackSerial;
    result->ackRequest = source->ackRequest;
    result->state      = source->state;
    result->error      = source->error;
    result->session    = source->session;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&source->serial, __ATOMIC_RELAXED) == serial)
    {
      result->serial = serial;
      return true;
    }
  }

  return false;
}

static bool lgmp_recoverySerialNewer(uint32_t serial, uint32_t reference)
{
  const uint32_t difference = serial - reference;
  return difference && difference < 0x80000000U;
}

static bool lgmp_recoveryLatestRequest(const KVMFRR * recovery,
    uint64_t session, KVMFRRRequest * result)
{
  bool found = false;
  for (unsigned i = 0; i < KVMFR_R_REQ_SLOTS; ++i)
  {
    KVMFRRRequest request;
    if (!lgmp_recoveryRequestSnapshot(&recovery->requests[i], &request) ||
        !request.serial || request.session != session)
      continue;

    if (!found || lgmp_recoverySerialNewer(request.serial, result->serial))
    {
      *result = request;
      found   = true;
    }
  }

  return found;
}

static LG_TransportStatus lgmp_getRecoveryInfo(LG_Transport * this,
    LG_RecoveryInfo * info)
{
  if (!info)
    return LG_TRANSPORT_ERROR;

  memset(info, 0, sizeof(*info));
  LG_LOCK(this->recoveryLock);
  const bool live = lgmp_refreshRecoveryLocked(this,
      lgmp_recoveryMagic(this->recovery));
  if (!live)
  {
    LG_UNLOCK(this->recoveryLock);
    return LG_TRANSPORT_UNAVAILABLE;
  }

  KVMFRRHeader header;
  KVMFRRInfo wireInfo;
  if (!lgmp_recoverySnapshot(this, &header, &wireInfo) ||
      header.session != this->recoverySession)
  {
    LG_UNLOCK(this->recoveryLock);
    return LG_TRANSPORT_UNAVAILABLE;
  }

  info->abiVersion   = header.abiVersion;
  info->instance     = header.session;
  info->heartbeat    = header.heartbeat;
  if (header.capabilities & KVMFR_R_CAP_DISPLAY)
    info->capabilities |= LG_RECOVERY_CAP_DISPLAY;

  memcpy(info->uuid, header.uuid, sizeof(info->uuid));
  for (unsigned i = 0; i < sizeof(info->uuid); ++i)
    info->uuidValid |= info->uuid[i] != 0;
  str_copy(info->producerVersion, sizeof(info->producerVersion),
      wireInfo.version, sizeof(wireInfo.version));

  info->versionCount = 2;
  str_copy(info->versions[0].component,
      sizeof(info->versions[0].component), "LGMP", sizeof("LGMP"));
  info->versions[0].version = header.lgmpVersion;
  str_copy(info->versions[1].component,
      sizeof(info->versions[1].component), "KVMFR", sizeof("KVMFR"));
  info->versions[1].version = header.kvmfrVersion;

  KVMFRRRequest request = {0};
  if (lgmp_recoveryLatestRequest(this->recovery, header.session, &request))
  {
    info->requestSerial = request.serial;
    info->request       = lgmp_recoveryRequestType(request.request);
  }

  KVMFRRStatus status;
  if (lgmp_recoveryStatusSnapshot(&this->recovery->status, &status) &&
      status.session == header.session)
  {
    info->ackSerial  = status.ackSerial;
    info->ackRequest = lgmp_recoveryRequestType(status.ackRequest);
    info->state      = lgmp_recoveryState(status.state);
    info->error      = lgmp_recoveryError(status.error);
  }

  LG_UNLOCK(this->recoveryLock);
  return LG_TRANSPORT_OK;
}

static LG_TransportStatus lgmp_requestRecovery(LG_Transport * this,
    LG_RecoveryRequest request, uint32_t * serial)
{
  uint32_t wireRequest;
  switch (request)
  {
    case LG_RECOVERY_REQ_NORMAL:
      wireRequest = KVMFR_R_REQ_NORMAL;
      break;
    case LG_RECOVERY_REQ_RECOVERY:
      wireRequest = KVMFR_R_REQ_RECOVERY;
      break;
    default:
      return LG_TRANSPORT_ERROR;
  }

  LG_LOCK(this->recoveryLock);
  if (!lgmp_refreshRecoveryLocked(this,
        lgmp_recoveryMagic(this->recovery)))
  {
    LG_UNLOCK(this->recoveryLock);
    return LG_TRANSPORT_UNAVAILABLE;
  }

  KVMFRRHeader header;
  KVMFRRInfo wireInfo;
  if (!lgmp_recoverySnapshot(this, &header, &wireInfo) ||
      header.session != this->recoverySession ||
      !(header.capabilities & KVMFR_R_CAP_DISPLAY))
  {
    LG_UNLOCK(this->recoveryLock);
    return LG_TRANSPORT_UNAVAILABLE;
  }

  uint32_t ticket = __atomic_add_fetch(&this->recovery->req.ticket, 2U,
      __ATOMIC_RELAXED);
  if (!ticket)
    ticket = __atomic_add_fetch(&this->recovery->req.ticket, 2U,
        __ATOMIC_RELAXED);
  if (!ticket || (ticket & KVMFR_R_REQ_WRITING))
  {
    LG_UNLOCK(this->recoveryLock);
    return LG_TRANSPORT_UNAVAILABLE;
  }

  KVMFRRRequest * destination = NULL;
  const uint32_t claimed = ticket | KVMFR_R_REQ_WRITING;
  for (unsigned i = 0; i < KVMFR_R_REQ_SLOTS; ++i)
  {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&this->recovery->requests[i].serial,
          &expected, claimed, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
    {
      destination = &this->recovery->requests[i];
      break;
    }
  }
  if (!destination)
  {
    LG_UNLOCK(this->recoveryLock);
    return LG_TRANSPORT_TIMEOUT;
  }

  destination->request = wireRequest;
  destination->session = header.session;

  KVMFRRHeader currentHeader;
  if (!lgmp_recoverySnapshot(this, &currentHeader, &wireInfo) ||
      currentHeader.session != header.session)
  {
    uint32_t expected = claimed;
    __atomic_compare_exchange_n(&destination->serial, &expected, 0, false,
        __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    LG_UNLOCK(this->recoveryLock);
    return LG_TRANSPORT_UNAVAILABLE;
  }

  uint32_t expected = claimed;
  if (!__atomic_compare_exchange_n(&destination->serial, &expected, ticket,
        false, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
  {
    LG_UNLOCK(this->recoveryLock);
    return LG_TRANSPORT_TIMEOUT;
  }
  if (serial)
    *serial = ticket;

  LG_UNLOCK(this->recoveryLock);
  return LG_TRANSPORT_OK;
}

static bool lgmp_supportsDMA(LG_Transport * this)
{
  return this->client && this->allowDMA && ivshmemHasDMA(&this->shm);
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

static void lgmp_waitPoll(LGEvent * event, unsigned interval)
{
  if (!interval)
    return;

  const uint64_t timeout = (uint64_t)interval * 1000U;
  if (timeout < TIMEOUT_INFINITE)
  {
    lgWaitEventNS(event, (unsigned)timeout);
    return;
  }

  const unsigned timeoutMS = interval / 1000U +
    (interval % 1000U != 0);
  lgWaitEvent(event, timeoutMS);
}

static LG_TransportStatus lgmp_process(PLGMPClientQueue * subscription,
    unsigned interval, LGEvent * wake, LGMPMessage * message)
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
        lgmp_waitPoll(wake, interval);
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
    lgmp_process(subscription, 0, NULL, &result->message);
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

  const uintptr_t base = (uintptr_t)this->shm.mem;
  const uintptr_t address = (uintptr_t)frame;
  if (address < base)
    return -1;

  const size_t position = address - base;
  if (position > this->lgmpSize ||
      frame->offset > this->lgmpSize - position ||
      sizeof(FrameBuffer) > this->lgmpSize - position - frame->offset)
    return -1;

  const size_t offset = position + frame->offset + sizeof(FrameBuffer);
  if (dataSize > this->lgmpSize - offset)
    return -1;

  dma->dataSize = dataSize;
  dma->fd       = ivshmemGetDMABuf(&this->shm, offset, dataSize);
  return dma->fd;
}

static void lgmp_releaseFrameLease(void * opaque, uint64_t handle);
static bool lgmp_frameTimingReady(const KVMFRFrame * frame);

static LG_TransportStatus lgmp_nextFrameLocked(LG_Transport * this,
    bool useDMA,
    LG_TransportFrame * result)
{
  lgmp_drainFrameLeasesLocked(this);
  if (!atomic_load_explicit(&this->connected, memory_order_acquire))
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

  const bool providerValid = lgmp_frameTimingReady(frame);
  const uint64_t providerStart = providerValid ? nanotime() : 0;

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
  lease->providerValid    = providerValid;
  lease->receiveTime      = 0;
  lease->prepareTime      = providerValid ? nanotime() - providerStart : 0;
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
  lgmp_retainVideoStatusLifetime(this);
  LG_LOCK(this->frameLock);
  bool subscribed[LGMP_FRAME_LEASE_COUNT];
  subscribed[0] = this->frameQueue != NULL;
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    subscribed[i + 1] = this->ownerFrameQueue[i] != NULL;
  const LG_TransportStatus status =
    lgmp_nextFrameLocked(this, useDMA, result);
  bool available = this->frameQueue != NULL;
  bool replaced = subscribed[0] != available;
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    const bool ownerAvailable = this->ownerFrameQueue[i] != NULL;
    available |= ownerAvailable;
    replaced |= subscribed[i + 1] != ownerAvailable;
  }
  if (status == LG_TRANSPORT_OK)
  {
    struct LGMPFrameLease * lease =
      lgmp_findFrameLeaseLocked(this, result->releaseHandle);
    DEBUG_ASSERT(lease);
  }
  if (status == LG_TRANSPORT_DISCONNECTED || status == LG_TRANSPORT_ERROR)
    available = false;
  LGMPVideoStatusDispatch dispatch;
  const LG_VideoStatus published = lgmp_updateVideoStatus(this,
      (LGMPVideoStatusUpdate)
  {
    .frame          = true,
    .frameAvailable = available,
    .frameReplaced  = replaced,
    .frameReason    = status,
  }, &dispatch);
  if (status == LG_TRANSPORT_OK)
    result->epoch = published.frame.epoch;
  LG_UNLOCK(this->frameLock);
  lgmp_dispatchVideoStatus(this, &dispatch);
  LG_TransportStatus resultStatus = status;
  if (lgmp_destroyRequested(this))
  {
    if (status == LG_TRANSPORT_OK)
      lgmp_releaseFrame(this, result);
    resultStatus = LG_TRANSPORT_DISCONNECTED;
  }
  else if ((status == LG_TRANSPORT_TIMEOUT ||
       status == LG_TRANSPORT_UNAVAILABLE ||
       (status == LG_TRANSPORT_ERROR && !available)) &&
      this->framePollInterval)
    lgmp_waitPoll(this->frameWake, this->framePollInterval);
  lgmp_releaseVideoStatusLifetime(this);
  return resultStatus;
}

static void lgmp_cancelFrameWait(LG_Transport * this)
{
  lgSignalEvent(this->frameWake);
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

  timing->providerValid = lease->providerValid;
  if (lease->providerValid)
  {
    timing->receiveTime = lease->receiveTime;
    timing->prepareTime = lease->prepareTime;
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
  timing->holdTime               = lease->frame->holdTime;
  timing->readyLeadTime          = lease->frame->readyLeadTime;
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
  lgmp_retainVideoStatusLifetime(this);
  if (!atomic_load_explicit(&this->connected, memory_order_acquire))
  {
    lgmp_publishVideoStatus(this, (LGMPVideoStatusUpdate)
    {
      .pointer       = true,
      .pointerReason = LG_TRANSPORT_DISCONNECTED,
    });
    lgmp_releaseVideoStatusLifetime(this);
    return LG_TRANSPORT_DISCONNECTED;
  }

  uint32_t           pointerFlags  = 0;
  size_t             shapeSize     = 0;
  size_t             transformSize = 0;
  LG_TransportStatus status        = LG_TRANSPORT_OK;
  PLGMPClientQueue   pointerQueue;
  LG_LOCK(this->pointerLock);
  const bool wasAvailable = this->pointerQueue != NULL;
  if (!this->pointerQueue)
  {
    status = lgmp_subscribe(this->client, LGMP_Q_POINTER,
        &this->pointerQueue);
  }
  pointerQueue = this->pointerQueue;
  LG_UNLOCK(this->pointerLock);
  if (status == LG_TRANSPORT_TIMEOUT)
    lgmp_waitPoll(this->pointerWake, 1000);
  if (status == LG_TRANSPORT_OK)
  {
    LGMPMessage message;
    PLGMPClientQueue processedQueue = pointerQueue;
    status = lgmp_process(&processedQueue, this->cursorPollInterval,
        this->pointerWake, &message);
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
  {
    LGMPVideoStatusDispatch dispatch;
    LG_LOCK(this->pointerLock);
    bool available = atomic_load_explicit(
        &this->connected, memory_order_acquire) &&
      this->pointerQueue != NULL;
    if (status == LG_TRANSPORT_DISCONNECTED || status == LG_TRANSPORT_ERROR)
      available = false;
    lgmp_updateVideoStatus(this, (LGMPVideoStatusUpdate)
    {
      .pointer          = true,
      .pointerAvailable = available,
      .pointerReplaced  = !wasAvailable && available,
      .pointerReason    = status,
    }, &dispatch);
    LG_UNLOCK(this->pointerLock);
    lgmp_dispatchVideoStatus(this, &dispatch);
    LG_TransportStatus resultStatus = status;
    if (lgmp_destroyRequested(this))
      resultStatus = LG_TRANSPORT_DISCONNECTED;
    else if (status == LG_TRANSPORT_ERROR && !available &&
        this->cursorPollInterval)
      lgmp_waitPoll(this->pointerWake, this->cursorPollInterval);
    lgmp_releaseVideoStatusLifetime(this);
    return resultStatus;
  }

  LGMPVideoStatusDispatch dispatch;
  LG_LOCK(this->pointerLock);
  const bool current =
    atomic_load_explicit(&this->connected, memory_order_acquire) &&
    this->pointerQueue == pointerQueue;
  const LG_VideoStatus published = lgmp_updateVideoStatus(this,
      (LGMPVideoStatusUpdate)
  {
    .pointer          = true,
    .pointerAvailable = current,
    .pointerReplaced  = current && !wasAvailable,
    .pointerReason    = current ? LG_TRANSPORT_OK :
      LG_TRANSPORT_DISCONNECTED,
  }, &dispatch);
  LG_UNLOCK(this->pointerLock);
  lgmp_dispatchVideoStatus(this, &dispatch);
  if (!current || lgmp_destroyRequested(this))
  {
    lgmp_releaseVideoStatusLifetime(this);
    return LG_TRANSPORT_DISCONNECTED;
  }

  const KVMFRCursor * cursor = (const KVMFRCursor *)this->pointerData;
  memset(result, 0, sizeof(*result));
  result->epoch = published.pointer.epoch;
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
  lgmp_releaseVideoStatusLifetime(this);
  return LG_TRANSPORT_OK;
}

static void lgmp_cancelPointerWait(LG_Transport * this)
{
  lgSignalEvent(this->pointerWake);
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

static const LG_InputOps * lgmp_getInputOps(LG_Transport * this,
    void ** opaque)
{
  *opaque = NULL;
  if (!atomic_load_explicit(&this->connected, memory_order_acquire) ||
      !atomic_load_explicit(&this->inputSupported, memory_order_acquire) ||
      !lgmpInput_connect(this->input, this->clientID))
    return NULL;

  *opaque = this->input;
  return lgmpInput_getOps();
}

static const LG_ClipboardOps * lgmp_getClipboardOps(
    LG_Transport * this, void ** opaque)
{
  *opaque = NULL;
  if (!atomic_load_explicit(&this->connected, memory_order_acquire) ||
      !atomic_load_explicit(
          &this->clipboardSupported, memory_order_acquire) ||
      !lgmpClipboard_connect(this->clipboard, this->clientID))
    return NULL;

  *opaque = this->clipboard;
  return lgmpClipboard_getOps();
}

static void lgmp_probeVideoStatus(LG_Transport * this)
{
  bool frameAvailable   = false;
  bool pointerAvailable = false;
  LGMPVideoStatusDispatch frameDispatch;
  LGMPVideoStatusDispatch pointerDispatch;

  LG_LOCK(this->frameLock);
  const bool frameConnected = atomic_load_explicit(
      &this->connected, memory_order_acquire);
  if (frameConnected)
  {
    lgmp_subscribe(this->client, LGMP_Q_FRAME, &this->frameQueue);
    frameAvailable = this->frameQueue != NULL;
    if (this->frameScheduleSupported)
      for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
      {
        lgmp_subscribe(this->client, LGMP_Q_FRAME_OWNER + i,
            &this->ownerFrameQueue[i]);
        frameAvailable |= this->ownerFrameQueue[i] != NULL;
      }
  }
  lgmp_updateVideoStatus(this, (LGMPVideoStatusUpdate)
  {
    .frame          = true,
    .frameAvailable = frameAvailable,
    .frameReason    = frameConnected ? LG_TRANSPORT_UNAVAILABLE :
      LG_TRANSPORT_DISCONNECTED,
  }, &frameDispatch);
  LG_UNLOCK(this->frameLock);
  lgmp_dispatchVideoStatus(this, &frameDispatch);

  LG_LOCK(this->pointerLock);
  const bool pointerConnected = atomic_load_explicit(
      &this->connected, memory_order_acquire);
  if (pointerConnected)
  {
    lgmp_subscribe(this->client, LGMP_Q_POINTER, &this->pointerQueue);
    pointerAvailable = this->pointerQueue != NULL;
  }
  lgmp_updateVideoStatus(this, (LGMPVideoStatusUpdate)
  {
    .pointer          = true,
    .pointerAvailable = pointerAvailable,
    .pointerReason    = pointerConnected ? LG_TRANSPORT_UNAVAILABLE :
      LG_TRANSPORT_DISCONNECTED,
  }, &pointerDispatch);
  LG_UNLOCK(this->pointerLock);
  lgmp_dispatchVideoStatus(this, &pointerDispatch);
}

static void lgmp_setVideoStatusListener(LG_Transport * this,
    LG_VideoStatusFn callback, void * callbackOpaque)
{
  LG_VideoStatus status;
  const bool nested = l_videoStatusContext != NULL;
  const unsigned ownCallbacks =
    lgmp_videoStatusOwnCallbacks(this);

  if (!nested)
    LG_LOCK(l_videoStatusCallbackLock);
  LG_LOCK_EXCLUSIVE(this->videoStatusLock);
  ++this->videoStatusListenerSerial;
  this->videoStatusCallback = NULL;
  this->videoStatusOpaque   = NULL;
  LG_UNLOCK_EXCLUSIVE(this->videoStatusLock);

  if (callback)
    lgmp_probeVideoStatus(this);

  LG_LOCK_EXCLUSIVE(this->videoStatusLock);
  this->videoStatusCallback = callback;
  this->videoStatusOpaque   = callbackOpaque;
  status = lgmp_videoStatusLocked(this);
  LG_UNLOCK_EXCLUSIVE(this->videoStatusLock);

  if (callback)
  {
    atomic_fetch_add_explicit(
        &this->videoStatusCallbacks, 1, memory_order_acq_rel);
    lgmp_callVideoStatusCallback(
        callback, callbackOpaque, this, &status);

    /* Releasing this callback's lifetime reference is the final access to the
     * instance unless this thread becomes the deferred destroy owner. */
    const bool destroy = lgmp_releaseVideoStatusCallback(this);
    if (!nested)
      LG_UNLOCK(l_videoStatusCallbackLock);
    if (destroy)
      lgmp_destroyNow(this);
    return;
  }

  if (nested)
    return;

  /* Publications accepted before unregistration retain lifetime references,
   * but will skip the cleared listener when they acquire serialization. */
  LG_UNLOCK(l_videoStatusCallbackLock);
  lgmp_waitVideoStatusCallbacks(this, ownCallbacks);
}

static const LG_FrameOps lgmpFrameOps =
{
  .setStatusListener = lgmp_setVideoStatusListener,
  .supportsDMA       = lgmp_supportsDMA,
  .attachRenderer    = lgmp_attachRenderer,
  .detachRenderer    = lgmp_detachRenderer,
  .nextFrame         = lgmp_nextFrame,
  .getFrameTiming    = lgmp_getFrameTiming,
  .releaseFrame      = lgmp_releaseFrame,
  .cancelFrameWait   = lgmp_cancelFrameWait,
  .stopFrame         = lgmp_stopFrame,
  .nextPointer       = lgmp_nextPointer,
  .releasePointer    = lgmp_releasePointer,
  .cancelPointerWait = lgmp_cancelPointerWait,
  .stopPointer       = lgmp_stopPointer,
};

static const LG_VideoOps lgmpVideoOps =
{
  .name  = "LGMP",
  .type  = LG_VIDEO_TYPE_FRAME,
  .frame = &lgmpFrameOps,
};

static const LG_VideoOps * lgmp_getVideoOps(LG_Transport * this)
{
  return &lgmpVideoOps;
}

const LG_TransportOps LGT_LGMP =
{
  .name             = "lgmp",
  .setup            = lgmp_setup,
  .create           = lgmp_create,
  .destroy          = lgmp_destroy,
  .connect            = lgmp_connect,
  .connectCancellable = lgmp_connectCancellable,
  .disconnect         = lgmp_disconnect,
  .sessionValid     = lgmp_sessionValid,
  .getVideoOps      = lgmp_getVideoOps,
  .getInputOps      = lgmp_getInputOps,
  .getClipboardOps  = lgmp_getClipboardOps,
  .getRecoveryInfo  = lgmp_getRecoveryInfo,
  .requestRecovery  = lgmp_requestRecovery,
  .sendControl      = lgmp_sendControl,
  .controlStatus    = lgmp_controlStatus,
};
