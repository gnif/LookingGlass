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

#include "frame_scheduler.h"
#include "main.h"

#include "common/debug.h"
#include "common/locking.h"
#include "common/time.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_SCHEDULER_LEASE_MS        1000U
#define FRAME_SCHEDULER_RENEW_NS        250000000ULL
#define FRAME_SCHEDULER_FEEDBACK_NS     50000000ULL
#define FRAME_SCHEDULER_TARGET_SLACK_NS 500000ULL
#define FRAME_SCHEDULER_MIN_PERIOD_NS   2000000ULL
#define FRAME_SCHEDULER_MAX_PERIOD_NS   1000000000ULL
#define FRAME_SCHEDULER_SAMPLE_COUNT    9U

static struct
{
  LG_Lock lock;

  _Atomic(bool)     supported;
  _Atomic(bool)     active;
  bool              controlPending;
  _Atomic(uint32_t) generation;
  _Atomic(uint64_t) period;
  uint64_t          lastSend;

  int64_t  phaseError;
  uint32_t feedbackFrameSerial;
  unsigned feedbackSamples;
  bool     feedbackDirty;

  LG_TransportControlToken controlToken;

  uint64_t renderSamples[FRAME_SCHEDULER_SAMPLE_COUNT];
  unsigned renderSampleIndex;
  unsigned renderSampleCount;
  uint64_t lastRender;
}
l_frameScheduler;

static int compareU64(const void * a, const void * b)
{
  const uint64_t lhs = *(const uint64_t *)a;
  const uint64_t rhs = *(const uint64_t *)b;
  return (lhs > rhs) - (lhs < rhs);
}

static uint64_t fallbackPeriod(void)
{
  uint64_t samples[FRAME_SCHEDULER_SAMPLE_COUNT];
  unsigned count;

  LG_LOCK(l_frameScheduler.lock);
  count = l_frameScheduler.renderSampleCount;
  memcpy(samples, l_frameScheduler.renderSamples,
      count * sizeof(*samples));
  LG_UNLOCK(l_frameScheduler.lock);

  if (count < 5)
    return 0;

  qsort(samples, count, sizeof(*samples), compareU64);
  const uint64_t period = samples[count / 2];
  if (samples[count * 3 / 4] - samples[count / 4] > period / 20)
    return 0;

  return period;
}

static uint64_t presentationPeriod(void)
{
  uint64_t period = 0;
  if (g_state.ds->getFramePeriod &&
      g_state.ds->getFramePeriod(&period))
    return period;

  return fallbackPeriod();
}

static bool controlReady(void)
{
  if (!l_frameScheduler.controlPending)
    return true;

  const LG_TransportStatus status = g_state.transportOps->controlStatus(
      g_state.transport, l_frameScheduler.controlToken);
  if (status == LG_TRANSPORT_UNAVAILABLE)
    return false;

  l_frameScheduler.controlPending = false;
  return status == LG_TRANSPORT_OK;
}

static bool sendSchedule(LG_TransportFrameScheduleFlags flags,
    uint64_t period)
{
  if (!controlReady())
    return false;

  int64_t  phaseError;
  uint32_t feedbackFrameSerial;
  LG_LOCK(l_frameScheduler.lock);
  phaseError          = l_frameScheduler.phaseError;
  feedbackFrameSerial = l_frameScheduler.feedbackFrameSerial;
  LG_UNLOCK(l_frameScheduler.lock);

  const LG_TransportControl control = {
    .type = LG_TRANSPORT_CONTROL_FRAME_SCHEDULE,
    .frameSchedule = {
      .generation          = l_frameScheduler.generation,
      .flags               = flags,
      .period              = period,
      .targetSlack         = FRAME_SCHEDULER_TARGET_SLACK_NS,
      .phaseError          = phaseError,
      .feedbackFrameSerial = feedbackFrameSerial,
      .lease               = FRAME_SCHEDULER_LEASE_MS,
    },
  };

  const LG_TransportStatus status = g_state.transportOps->sendControl(
      g_state.transport, &control, &l_frameScheduler.controlToken);
  if (status == LG_TRANSPORT_UNAVAILABLE)
    return false;
  if (status != LG_TRANSPORT_OK)
  {
    DEBUG_WARN("Frame-schedule control failed with status %d", status);
    return false;
  }

  l_frameScheduler.controlPending = true;
  LG_LOCK(l_frameScheduler.lock);
  if (l_frameScheduler.feedbackFrameSerial == feedbackFrameSerial)
    l_frameScheduler.feedbackDirty = false;
  LG_UNLOCK(l_frameScheduler.lock);
  return true;
}

void frameScheduler_init(void)
{
  memset(&l_frameScheduler, 0, sizeof(l_frameScheduler));
  LG_LOCK_INIT(l_frameScheduler.lock);
}

void frameScheduler_free(void)
{
  LG_LOCK_FREE(l_frameScheduler.lock);
}

void frameScheduler_start(LG_TransportFeatureFlags features)
{
  l_frameScheduler.supported =
    features & LG_TRANSPORT_FEATURE_FRAME_SCHEDULE;
  l_frameScheduler.active         = false;
  l_frameScheduler.controlPending = false;
  l_frameScheduler.lastSend       = 0;
  ++l_frameScheduler.generation;

  LG_LOCK(l_frameScheduler.lock);
  l_frameScheduler.phaseError          = 0;
  l_frameScheduler.feedbackFrameSerial = 0;
  l_frameScheduler.feedbackSamples     = 0;
  l_frameScheduler.feedbackDirty       = false;
  LG_UNLOCK(l_frameScheduler.lock);
}

void frameScheduler_stop(void)
{
  if (l_frameScheduler.supported && l_frameScheduler.active)
    sendSchedule(LG_TRANSPORT_FRAME_SCHEDULE_RELEASE, 0);

  l_frameScheduler.supported      = false;
  l_frameScheduler.active         = false;
  l_frameScheduler.controlPending = false;
}

void frameScheduler_update(void)
{
  if (!l_frameScheduler.supported)
    return;

  const uint64_t now = nanotime();
  uint64_t period    = presentationPeriod();
  if (period < FRAME_SCHEDULER_MIN_PERIOD_NS ||
      period > FRAME_SCHEDULER_MAX_PERIOD_NS)
  {
    period = l_frameScheduler.period;
    if (period < FRAME_SCHEDULER_MIN_PERIOD_NS ||
        period > FRAME_SCHEDULER_MAX_PERIOD_NS)
      return;
  }

  bool reset = !l_frameScheduler.period;
  if (!reset)
  {
    const uint64_t delta = l_frameScheduler.period > period ?
      l_frameScheduler.period - period : period - l_frameScheduler.period;
    reset = delta > l_frameScheduler.period / 20;
  }

  if (reset)
  {
    l_frameScheduler.period = period;
    ++l_frameScheduler.generation;
    LG_LOCK(l_frameScheduler.lock);
    l_frameScheduler.phaseError          = 0;
    l_frameScheduler.feedbackFrameSerial = 0;
    l_frameScheduler.feedbackSamples     = 0;
    l_frameScheduler.feedbackDirty       = false;
    LG_UNLOCK(l_frameScheduler.lock);
  }
  else
    l_frameScheduler.period =
      (l_frameScheduler.period * 7 + period) / 8;

  if (l_frameScheduler.active && !reset)
  {
    LG_LOCK(l_frameScheduler.lock);
    const bool feedbackDirty = l_frameScheduler.feedbackDirty;
    LG_UNLOCK(l_frameScheduler.lock);
    const uint64_t interval = feedbackDirty ?
      FRAME_SCHEDULER_FEEDBACK_NS : FRAME_SCHEDULER_RENEW_NS;
    if (now - l_frameScheduler.lastSend < interval)
      return;
  }

  LG_TransportFrameScheduleFlags flags =
    LG_TRANSPORT_FRAME_SCHEDULE_ACTIVE;
  if (reset)
    flags |= LG_TRANSPORT_FRAME_SCHEDULE_RESET;

  if (sendSchedule(flags, l_frameScheduler.period))
  {
    l_frameScheduler.active   = true;
    l_frameScheduler.lastSend = now;
  }
}

void frameScheduler_observeRender(uint64_t timestamp)
{
  LG_LOCK(l_frameScheduler.lock);
  if (l_frameScheduler.lastRender &&
      timestamp > l_frameScheduler.lastRender)
  {
    l_frameScheduler.renderSamples[l_frameScheduler.renderSampleIndex] =
      timestamp - l_frameScheduler.lastRender;
    l_frameScheduler.renderSampleIndex =
      (l_frameScheduler.renderSampleIndex + 1) %
        FRAME_SCHEDULER_SAMPLE_COUNT;
    if (l_frameScheduler.renderSampleCount < FRAME_SCHEDULER_SAMPLE_COUNT)
      ++l_frameScheduler.renderSampleCount;
  }
  l_frameScheduler.lastRender = timestamp;
  LG_UNLOCK(l_frameScheduler.lock);
}

void frameScheduler_feedback(uint64_t frameSerial, uint32_t generation,
    uint64_t measuredPhase)
{
  if (!generation)
    return;

  LG_LOCK(l_frameScheduler.lock);
  if (!l_frameScheduler.supported || !l_frameScheduler.active ||
      generation != l_frameScheduler.generation)
  {
    LG_UNLOCK(l_frameScheduler.lock);
    return;
  }

  const int64_t period = (int64_t)l_frameScheduler.period;
  if (!period)
  {
    LG_UNLOCK(l_frameScheduler.lock);
    return;
  }

  int64_t error = measuredPhase > FRAME_SCHEDULER_TARGET_SLACK_NS ?
    (int64_t)(measuredPhase - FRAME_SCHEDULER_TARGET_SLACK_NS) :
    -(int64_t)(FRAME_SCHEDULER_TARGET_SLACK_NS - measuredPhase);
  error %= period;
  if (error > period / 2)
    error -= period;
  else if (error < -period / 2)
    error += period;

  if (!l_frameScheduler.feedbackSamples)
    l_frameScheduler.phaseError = error;
  else
    l_frameScheduler.phaseError =
      (l_frameScheduler.phaseError * 7 + error) / 8;
  if (l_frameScheduler.feedbackSamples < 32)
    ++l_frameScheduler.feedbackSamples;

  l_frameScheduler.feedbackFrameSerial = (uint32_t)frameSerial;
  l_frameScheduler.feedbackDirty       = true;
  LG_UNLOCK(l_frameScheduler.lock);
}
