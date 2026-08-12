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

#include "transport_fallback.h"

#include "audio.h"
#include "clipboard.h"
#include "input.h"

#include "common/debug.h"
#include "common/event.h"
#include "common/locking.h"
#include "common/thread.h"
#include "common/util.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define RETRY_INITIAL_MS 250U
#define RETRY_MAX_MS    5000U
#define SESSION_POLL_MS  100U

struct LG_TransportFallback
{
  char * transportName;

  LG_SwSurfaceEventOps surfaceEvents;
  void                * surfaceOpaque;
  LG_TransportFallbackEventOps eventOps;
  void                         * eventOpaque;

  LGThread * thread;
  LGEvent  * wakeEvent;
  LG_RWLock  lock;
  LG_Lock    providerLock;
  LG_Lock    eventLock;

  atomic_bool stop;
  atomic_bool ready;
  atomic_bool admitted;

  LG_TransportInstance  transport;
  LG_TransportSession   session;
  const LG_VideoOps   * videoOps;
  uint64_t              connectionSerial;
  bool                  attached;
  bool                  connected;
  bool                  providersPublished;
  bool                  connectedReported;
  bool                  disconnectedReported;
  bool                  closing;
  bool                  videoRequested;
  bool                  videoActive;

  bool    primaryUUIDValid;
  uint8_t primaryUUID[16];

  bool    mismatchReported;
  uint8_t mismatchPrimary[16];
  uint8_t mismatchFallback[16];
};

static bool applyVideoRequest(LG_TransportFallback * fallback);

static bool connectCancelled(void * opaque)
{
  const LG_TransportFallback * fallback = opaque;
  return atomic_load_explicit(&fallback->stop, memory_order_acquire);
}

static bool uuidMismatchLocked(const LG_TransportFallback * fallback)
{
  return fallback->primaryUUIDValid && fallback->session.uuidValid &&
    memcmp(fallback->primaryUUID, fallback->session.uuid,
        sizeof(fallback->primaryUUID)) != 0;
}

static bool uuidAdmittedLocked(const LG_TransportFallback * fallback)
{
  return !uuidMismatchLocked(fallback);
}

static bool recordMismatchLocked(LG_TransportFallback * fallback,
    uint8_t primary[16], uint8_t remote[16])
{
  if (!uuidMismatchLocked(fallback))
    return false;

  const bool duplicate = fallback->mismatchReported &&
    memcmp(fallback->mismatchPrimary, fallback->primaryUUID,
        sizeof(fallback->mismatchPrimary)) == 0 &&
    memcmp(fallback->mismatchFallback, fallback->session.uuid,
        sizeof(fallback->mismatchFallback)) == 0;
  if (duplicate)
    return false;

  memcpy(fallback->mismatchPrimary, fallback->primaryUUID,
      sizeof(fallback->mismatchPrimary));
  memcpy(fallback->mismatchFallback, fallback->session.uuid,
      sizeof(fallback->mismatchFallback));
  memcpy(primary, fallback->primaryUUID, sizeof(fallback->primaryUUID));
  memcpy(remote, fallback->session.uuid, sizeof(fallback->session.uuid));
  fallback->mismatchReported = true;
  return true;
}

static bool sessionLive(const LG_TransportFallback * fallback)
{
  return fallback->connected && fallback->transport.ops &&
    fallback->transport.ops->sessionValid(fallback->transport.handle);
}

static void notifyConnected(LG_TransportFallback * fallback)
{
  LG_LOCK(fallback->eventLock);
  LG_LOCK_EXCLUSIVE(fallback->lock);
  const bool report =
    atomic_load_explicit(&fallback->ready, memory_order_acquire) &&
    atomic_load_explicit(&fallback->admitted, memory_order_acquire) &&
    uuidAdmittedLocked(fallback) && !fallback->closing &&
    !fallback->disconnectedReported;
  fallback->connectedReported = report;
  const LG_TransportSession session = fallback->session;
  LG_UNLOCK_EXCLUSIVE(fallback->lock);

  if (report && fallback->eventOps.connected)
    fallback->eventOps.connected(fallback->eventOpaque, &session);
  LG_UNLOCK(fallback->eventLock);
}

static void notifyDisconnected(
    LG_TransportFallback * fallback, bool requested)
{
  if (!requested)
    return;

  LG_LOCK(fallback->eventLock);
  LG_LOCK_EXCLUSIVE(fallback->lock);
  const bool report = !fallback->disconnectedReported;
  if (report)
    fallback->disconnectedReported = true;
  LG_UNLOCK_EXCLUSIVE(fallback->lock);

  if (report && fallback->eventOps.disconnected)
    fallback->eventOps.disconnected(fallback->eventOpaque);
  LG_UNLOCK(fallback->eventLock);
}

static void notifyLost(LG_TransportFallback * fallback, bool requested)
{
  if (!requested || !fallback->eventOps.lost)
    return;

  LG_LOCK(fallback->eventLock);
  fallback->eventOps.lost(fallback->eventOpaque);
  LG_UNLOCK(fallback->eventLock);
}

static void unpublishProviders(LG_TransportFallback * fallback, bool live)
{
  LG_LOCK(fallback->providerLock);
  if (!fallback->providersPublished)
  {
    LG_UNLOCK(fallback->providerLock);
    return;
  }

  if (live)
  {
    lgInput_setFallback(NULL, NULL);
    lgAudio_setFallback(NULL, NULL);
    lgClipboard_setFallback(NULL, NULL);
  }
  else
  {
    lgInput_dropFallback();
    lgAudio_dropFallback();
    lgClipboard_dropFallback();
  }
  fallback->providersPublished = false;
  LG_UNLOCK(fallback->providerLock);
}

static void closeVideoAdmission(LG_TransportFallback * fallback)
{
  LG_LOCK_EXCLUSIVE(fallback->lock);
  atomic_store_explicit(&fallback->ready, false, memory_order_release);
  atomic_store_explicit(&fallback->admitted, false, memory_order_release);
  fallback->closing = true;
  LG_UNLOCK_EXCLUSIVE(fallback->lock);
}

static bool cleanupConnection(LG_TransportFallback * fallback,
    bool knownDead)
{
  closeVideoAdmission(fallback);

  LG_LOCK_EXCLUSIVE(fallback->lock);
  const bool reportDisconnected = fallback->connectedReported;
  fallback->connectedReported = false;
  LG_UNLOCK_EXCLUSIVE(fallback->lock);

  const bool live = !knownDead && sessionLive(fallback);
  unpublishProviders(fallback, live);

  if (fallback->attached)
  {
    if (live && fallback->videoActive)
      fallback->videoOps->swSurface->setActive(
          fallback->transport.handle, false);
    fallback->videoActive = false;
    fallback->videoOps->swSurface->detach(fallback->transport.handle);
    fallback->attached = false;
  }

  if (fallback->transport.ops)
    fallback->transport.ops->disconnect(fallback->transport.handle);
  lgTransport_destroy(&fallback->transport);

  LG_LOCK_EXCLUSIVE(fallback->lock);
  fallback->session   = (LG_TransportSession) { 0 };
  fallback->videoOps  = NULL;
  fallback->connected = false;
  fallback->closing   = false;
  ++fallback->connectionSerial;
  LG_UNLOCK_EXCLUSIVE(fallback->lock);
  return reportDisconnected;
}

static bool publishProviders(LG_TransportFallback * fallback)
{
  LG_LOCK(fallback->providerLock);
  LG_LOCK_SHARED(fallback->lock);
  const bool admitted = atomic_load_explicit(
      &fallback->admitted, memory_order_acquire) &&
    uuidAdmittedLocked(fallback) && !fallback->closing;
  if (!admitted)
  {
    LG_UNLOCK_SHARED(fallback->lock);
    LG_UNLOCK(fallback->providerLock);
    return false;
  }

  void * inputOpaque = NULL;
  const LG_InputOps * inputOps = fallback->transport.ops->getInputOps ?
    fallback->transport.ops->getInputOps(
        fallback->transport.handle, &inputOpaque) : NULL;
  lgInput_setFallback(inputOps, inputOpaque);

  void * audioOpaque = NULL;
  const LG_AudioOps * audioOps = fallback->transport.ops->getAudioOps ?
    fallback->transport.ops->getAudioOps(
        fallback->transport.handle, &audioOpaque) : NULL;
  lgAudio_setFallback(audioOps, audioOpaque);

  void * clipboardOpaque = NULL;
  const LG_ClipboardOps * clipboardOps =
    fallback->transport.ops->getClipboardOps ?
      fallback->transport.ops->getClipboardOps(
          fallback->transport.handle, &clipboardOpaque) : NULL;
  lgClipboard_setFallback(clipboardOps, clipboardOpaque);
  fallback->providersPublished = true;
  LG_UNLOCK_SHARED(fallback->lock);
  LG_UNLOCK(fallback->providerLock);
  return true;
}

static bool admitConnection(LG_TransportFallback * fallback,
    bool * reportMismatch, uint8_t primaryUUID[16],
    uint8_t fallbackUUID[16])
{
  LG_LOCK_EXCLUSIVE(fallback->lock);
  const bool reject = uuidMismatchLocked(fallback);
  if (reject)
    *reportMismatch = recordMismatchLocked(
        fallback, primaryUUID, fallbackUUID);
  const bool stop = atomic_load_explicit(
      &fallback->stop, memory_order_acquire);
  const bool admitted = !reject && !stop && !fallback->closing;
  if (admitted)
    atomic_store_explicit(
        &fallback->admitted, true, memory_order_release);
  LG_UNLOCK_EXCLUSIVE(fallback->lock);
  return admitted;
}

static bool publishConnection(LG_TransportFallback * fallback,
    bool * reportMismatch, uint8_t primaryUUID[16],
    uint8_t fallbackUUID[16])
{
  if (!admitConnection(fallback, reportMismatch,
        primaryUUID, fallbackUUID))
    return false;

  if (!publishProviders(fallback))
    return false;

  for (;;)
  {
    LG_LOCK_EXCLUSIVE(fallback->lock);
    const bool reject = uuidMismatchLocked(fallback);
    if (reject)
      *reportMismatch = recordMismatchLocked(
          fallback, primaryUUID, fallbackUUID);
    const bool admitted = uuidAdmittedLocked(fallback);
    const bool stop = atomic_load_explicit(
        &fallback->stop, memory_order_acquire);
    const bool closing = fallback->closing;
    const bool requested = fallback->videoRequested;
    LG_UNLOCK_EXCLUSIVE(fallback->lock);

    if (reject || !admitted || stop || closing)
      return false;
    if (requested == fallback->videoActive)
      break;

    if (!fallback->videoOps->swSurface->setActive(
          fallback->transport.handle, requested))
      break;
    fallback->videoActive = requested;
  }

  LG_LOCK_EXCLUSIVE(fallback->lock);
  const bool finalReject = uuidMismatchLocked(fallback);
  if (finalReject)
    *reportMismatch = recordMismatchLocked(
        fallback, primaryUUID, fallbackUUID);
  const bool finalAdmitted = uuidAdmittedLocked(fallback);
  const bool finalStop = atomic_load_explicit(
      &fallback->stop, memory_order_acquire);
  if (!finalReject && finalAdmitted && !finalStop && !fallback->closing)
  {
    fallback->mismatchReported = false;
    atomic_store_explicit(&fallback->ready, true, memory_order_release);
  }
  const bool published = !finalReject && finalAdmitted && !finalStop &&
    !fallback->closing;
  LG_UNLOCK_EXCLUSIVE(fallback->lock);
  return published;
}

static bool connectFallback(LG_TransportFallback * fallback)
{
  LG_TransportInstance transport = { 0 };
  if (!lgTransport_create(fallback->transportName, &transport))
  {
    DEBUG_ERROR("Failed to create fallback transport %s",
        fallback->transportName);
    return false;
  }

  const LG_VideoOps * videoOps = transport.ops->getVideoOps ?
    transport.ops->getVideoOps(transport.handle) : NULL;
  if (!videoOps || videoOps->type != LG_VIDEO_TYPE_SW_SURFACE ||
      !videoOps->swSurface || !videoOps->swSurface->attach ||
      !videoOps->swSurface->detach || !videoOps->swSurface->setActive)
  {
    DEBUG_ERROR("Fallback transport %s does not provide a software surface",
        fallback->transportName);
    lgTransport_destroy(&transport);
    return false;
  }

  if (!videoOps->swSurface->attach(transport.handle,
        &fallback->surfaceEvents, fallback->surfaceOpaque))
  {
    DEBUG_ERROR("Failed to attach fallback software surface");
    lgTransport_destroy(&transport);
    return false;
  }

  LG_LOCK_EXCLUSIVE(fallback->lock);
  fallback->transport = transport;
  fallback->videoOps  = videoOps;
  fallback->attached  = true;
  LG_UNLOCK_EXCLUSIVE(fallback->lock);

  LG_TransportSession session = { 0 };
  const LG_TransportStatus status = transport.ops->connectCancellable ?
    transport.ops->connectCancellable(transport.handle, &session,
        connectCancelled, fallback) :
    transport.ops->connect(transport.handle, &session);

  if (status != LG_TRANSPORT_OK)
  {
    if (!atomic_load_explicit(&fallback->stop, memory_order_acquire))
      DEBUG_ERROR("Fallback transport %s failed to connect: %d",
          fallback->transportName, status);
    cleanupConnection(fallback, false);
    return false;
  }

  LG_LOCK_EXCLUSIVE(fallback->lock);
  fallback->connected            = true;
  fallback->session              = session;
  fallback->disconnectedReported = false;
  ++fallback->connectionSerial;
  LG_UNLOCK_EXCLUSIVE(fallback->lock);

  bool reportMismatch = false;
  uint8_t primaryUUID[16];
  uint8_t fallbackUUID[16];
  if (!publishConnection(fallback, &reportMismatch,
        primaryUUID, fallbackUUID))
  {
    cleanupConnection(fallback, false);
    if (reportMismatch && fallback->eventOps.uuidMismatch)
      fallback->eventOps.uuidMismatch(
          fallback->eventOpaque, primaryUUID, fallbackUUID);
    return false;
  }

  notifyConnected(fallback);

  bool lost = false;
  while (!atomic_load_explicit(&fallback->stop, memory_order_acquire))
  {
    lgWaitEvent(fallback->wakeEvent, SESSION_POLL_MS);
    if (atomic_load_explicit(&fallback->stop, memory_order_acquire))
      break;
    applyVideoRequest(fallback);

    LG_LOCK_EXCLUSIVE(fallback->lock);
    const bool reject = uuidMismatchLocked(fallback);
    if (reject)
      reportMismatch = recordMismatchLocked(
          fallback, primaryUUID, fallbackUUID);
    const bool admitted = uuidAdmittedLocked(fallback);
    const bool closing = fallback->closing;
    LG_UNLOCK_EXCLUSIVE(fallback->lock);
    if (reject || !admitted || closing)
      break;
    if (!sessionLive(fallback))
    {
      lost = !atomic_load_explicit(
          &fallback->stop, memory_order_acquire);
      break;
    }
  }

  const bool reportDisconnect = cleanupConnection(fallback, lost);

  if (reportMismatch && fallback->eventOps.uuidMismatch)
    fallback->eventOps.uuidMismatch(
        fallback->eventOpaque, primaryUUID, fallbackUUID);
  const bool reportLost = lost && reportDisconnect && !atomic_load_explicit(
      &fallback->stop, memory_order_acquire);
  notifyLost(fallback, reportLost);
  notifyDisconnected(fallback, reportDisconnect);
  return true;
}

static int fallbackThread(void * opaque)
{
  LG_TransportFallback * fallback = opaque;
  unsigned int retry = RETRY_INITIAL_MS;

  while (!atomic_load_explicit(&fallback->stop, memory_order_acquire))
  {
    if (connectFallback(fallback))
      retry = RETRY_INITIAL_MS;

    if (atomic_load_explicit(&fallback->stop, memory_order_acquire))
      break;

    lgWaitEvent(fallback->wakeEvent, retry);
    retry = min(retry * 2U, RETRY_MAX_MS);
  }

  const bool reportDisconnect = cleanupConnection(fallback, false);
  notifyDisconnected(fallback, reportDisconnect);
  return 0;
}

bool lgTransportFallback_start(const char * transportName,
    const LG_SwSurfaceEventOps * surfaceEvents, void * surfaceOpaque,
    const LG_TransportFallbackEventOps * eventOps, void * eventOpaque,
    const uint8_t primaryUUID[16],
    LG_TransportFallback ** result)
{
  if (!result)
    return false;

  *result = NULL;
  if (!transportName || !*transportName || !surfaceEvents ||
      !lgTransport_isValid(transportName))
    return false;

  LG_TransportFallback * fallback = calloc(1, sizeof(*fallback));
  if (!fallback)
    return false;

  LG_RWLOCK_INIT(fallback->lock);
  LG_LOCK_INIT(fallback->providerLock);
  LG_LOCK_INIT(fallback->eventLock);
  atomic_init(&fallback->stop, false);
  atomic_init(&fallback->ready, false);
  atomic_init(&fallback->admitted, false);

  fallback->transportName = strdup(transportName);
  if (!fallback->transportName)
    goto fail;

  fallback->surfaceEvents = *surfaceEvents;
  fallback->surfaceOpaque = surfaceOpaque;
  if (eventOps)
    fallback->eventOps = *eventOps;
  fallback->eventOpaque = eventOpaque;
  if (primaryUUID)
  {
    memcpy(fallback->primaryUUID, primaryUUID,
        sizeof(fallback->primaryUUID));
    fallback->primaryUUIDValid = true;
  }

  fallback->wakeEvent = lgCreateEvent(true, 0);
  if (!fallback->wakeEvent)
    goto fail;

  *result = fallback;
  if (!lgCreateThread("transportFallback", fallbackThread,
        fallback, &fallback->thread))
    goto fail;

  return true;

fail:
  *result = NULL;
  if (fallback->wakeEvent)
    lgFreeEvent(fallback->wakeEvent);
  LG_LOCK_FREE(fallback->eventLock);
  LG_LOCK_FREE(fallback->providerLock);
  LG_RWLOCK_FREE(fallback->lock);
  free(fallback->transportName);
  free(fallback);
  return false;
}

void lgTransportFallback_stop(LG_TransportFallback ** fallbackPtr)
{
  if (!fallbackPtr || !*fallbackPtr)
    return;

  LG_TransportFallback * fallback = *fallbackPtr;
  *fallbackPtr = NULL;

  atomic_store_explicit(&fallback->stop, true, memory_order_release);
  lgSignalEvent(fallback->wakeEvent);

  LGThread * thread = fallback->thread;
  fallback->thread = NULL;
  if (!lgJoinThread(thread, NULL))
  {
    DEBUG_ERROR("Failed to stop fallback transport worker; preserving state");
    return;
  }

  lgFreeEvent(fallback->wakeEvent);
  LG_LOCK_FREE(fallback->eventLock);
  LG_LOCK_FREE(fallback->providerLock);
  LG_RWLOCK_FREE(fallback->lock);
  free(fallback->transportName);
  free(fallback);
}

bool lgTransportFallback_ready(const LG_TransportFallback * fallback)
{
  return fallback && atomic_load_explicit(
      &fallback->ready, memory_order_acquire);
}

bool lgTransportFallback_admitted(const LG_TransportFallback * fallback)
{
  return fallback && atomic_load_explicit(
      &fallback->admitted, memory_order_acquire);
}

bool lgTransportFallback_videoRequested(
    LG_TransportFallback * fallback)
{
  if (!fallback)
    return false;

  LG_LOCK_SHARED(fallback->lock);
  const bool requested = fallback->videoRequested;
  LG_UNLOCK_SHARED(fallback->lock);
  return requested;
}

static bool applyVideoRequest(LG_TransportFallback * fallback)
{
  for (;;)
  {
    uint64_t connectionSerial;
    LG_Transport * transport;
    const LG_SwSurfaceOps * surfaceOps;
    bool requested;

    LG_LOCK_EXCLUSIVE(fallback->lock);
    if (!atomic_load_explicit(&fallback->ready, memory_order_acquire) ||
        fallback->closing)
    {
      LG_UNLOCK_EXCLUSIVE(fallback->lock);
      return false;
    }

    requested = fallback->videoRequested;
    if (fallback->videoActive == requested)
    {
      LG_UNLOCK_EXCLUSIVE(fallback->lock);
      return true;
    }

    connectionSerial    = fallback->connectionSerial;
    transport           = fallback->transport.handle;
    surfaceOps          = fallback->videoOps->swSurface;
    LG_UNLOCK_EXCLUSIVE(fallback->lock);

    const bool result = surfaceOps->setActive(transport, requested);

    LG_LOCK_EXCLUSIVE(fallback->lock);
    const bool current = fallback->connectionSerial == connectionSerial;
    if (current && result)
      fallback->videoActive = requested;
    const bool accepted = current && !fallback->closing && result;
    const bool retry = current && !fallback->closing &&
      fallback->videoRequested != requested;
    LG_UNLOCK_EXCLUSIVE(fallback->lock);

    if (!retry)
      return accepted;
  }
}

void lgTransportFallback_requestVideoActive(
    LG_TransportFallback * fallback, bool active)
{
  if (!fallback)
    return;

  LG_LOCK_EXCLUSIVE(fallback->lock);
  fallback->videoRequested = active;
  LG_UNLOCK_EXCLUSIVE(fallback->lock);
  lgSignalEvent(fallback->wakeEvent);
}

static bool revokeAdmissionLocked(LG_TransportFallback * fallback)
{
  atomic_store_explicit(&fallback->ready, false, memory_order_release);
  atomic_store_explicit(&fallback->admitted, false, memory_order_release);
  if (!fallback->connected)
    return false;

  fallback->connectedReported = false;
  fallback->closing           = true;
  return true;
}

void lgTransportFallback_setPrimaryUUID(
    LG_TransportFallback * fallback, const uint8_t uuid[16])
{
  if (!fallback || !uuid)
    return;

  bool revoked          = false;
  bool notifyDisconnect = false;
  bool reportMismatch   = false;
  uint8_t primaryUUID[16];
  uint8_t fallbackUUID[16];

  LG_LOCK_EXCLUSIVE(fallback->lock);
  const bool changed = !fallback->primaryUUIDValid ||
    memcmp(fallback->primaryUUID, uuid,
        sizeof(fallback->primaryUUID)) != 0;
  if (changed)
  {
    memcpy(fallback->primaryUUID, uuid, sizeof(fallback->primaryUUID));
    fallback->primaryUUIDValid = true;
    if (uuidMismatchLocked(fallback))
    {
      revoked = true;
      notifyDisconnect = revokeAdmissionLocked(fallback);
      reportMismatch = recordMismatchLocked(
          fallback, primaryUUID, fallbackUUID);
    }
  }
  LG_UNLOCK_EXCLUSIVE(fallback->lock);

  if (changed)
    lgSignalEvent(fallback->wakeEvent);
  if (revoked)
    unpublishProviders(fallback, false);
  notifyDisconnected(fallback, notifyDisconnect);
  if (reportMismatch && fallback->eventOps.uuidMismatch)
    fallback->eventOps.uuidMismatch(
        fallback->eventOpaque, primaryUUID, fallbackUUID);
}

void lgTransportFallback_clearPrimaryUUID(
    LG_TransportFallback * fallback)
{
  if (!fallback)
    return;

  LG_LOCK_EXCLUSIVE(fallback->lock);
  const bool changed = fallback->primaryUUIDValid;
  if (changed)
  {
    fallback->primaryUUIDValid = false;
    fallback->mismatchReported = false;
  }
  LG_UNLOCK_EXCLUSIVE(fallback->lock);

  if (changed)
    lgSignalEvent(fallback->wakeEvent);
}
