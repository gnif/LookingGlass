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

#include <common/KVMFR.h>

#include <string.h>

#define FRAME_SCHEDULER_LEASE_MS          1000U
#define FRAME_SCHEDULER_RENEW_NS          250000000ULL
#define FRAME_SCHEDULER_FEEDBACK_NS       50000000ULL
#define FRAME_SCHEDULER_TICK_GRACE_NS     500000000ULL
#define FRAME_SCHEDULER_TARGET_SLACK_NS   1000000ULL
#define FRAME_SCHEDULER_MIN_PERIOD_NS     2000000ULL
#define FRAME_SCHEDULER_MAX_PERIOD_NS     1000000000ULL
#define FRAME_SCHEDULER_CONTROL_RETRY_NS  10000000ULL
#define FRAME_SCHEDULER_CONTROL_RETRY_MAX 250000000ULL

static void lgFrameSchedulerControlBackoff(
    LGFrameScheduler * scheduler, uint64_t now)
{
  uint64_t delay = scheduler->controlRetryDelay;
  if (!delay)
    delay = FRAME_SCHEDULER_CONTROL_RETRY_NS;

  scheduler->nextControlCheck = now + delay;
  scheduler->controlRetryDelay =
    delay < FRAME_SCHEDULER_CONTROL_RETRY_MAX / 2 ?
      delay * 2 : FRAME_SCHEDULER_CONTROL_RETRY_MAX;
}

static bool lgFrameSchedulerControlReady(LGFrameScheduler * scheduler,
    PLGMPClientQueue queue, uint64_t now)
{
  if (!scheduler->controlPending)
    return now >= scheduler->nextControlCheck;

  if (now < scheduler->nextControlCheck)
    return false;

  uint32_t serial;
  const LGMP_STATUS status = lgmpClientGetSerial(queue, &serial);
  if (status != LGMP_OK ||
      (int32_t)(serial - scheduler->controlSerial) < 0)
  {
    if (status != LGMP_OK)
      scheduler->controlFaulted = true;
    lgFrameSchedulerControlBackoff(scheduler, now);
    return false;
  }

  scheduler->controlPending    = false;
  scheduler->nextControlCheck  = 0;
  scheduler->controlRetryDelay = FRAME_SCHEDULER_CONTROL_RETRY_NS;
  if (scheduler->controlFaulted)
  {
    scheduler->controlFaulted   = false;
    scheduler->immediatePending = true;
  }
  return true;
}

static bool lgFrameSchedulerSend(LGFrameScheduler * scheduler,
    PLGMPClientQueue queue, const KVMFRFrameSchedule * message,
    uint64_t now)
{
  uint32_t serial;
  if (lgmpClientSendData(
        queue, message, sizeof(*message), &serial) != LGMP_OK)
  {
    lgFrameSchedulerControlBackoff(scheduler, now);
    return false;
  }

  scheduler->controlPending    = true;
  scheduler->controlSerial     = serial;
  scheduler->nextControlCheck  =
    now + FRAME_SCHEDULER_CONTROL_RETRY_NS;
  scheduler->controlRetryDelay = FRAME_SCHEDULER_CONTROL_RETRY_NS;
  return true;
}

static void lgFrameSchedulerResetFeedback(LGFrameScheduler * scheduler)
{
  scheduler->phaseError             = 0;
  scheduler->feedbackFrameSerial    = 0;
  scheduler->feedbackScheduleEpoch  = 0;
  scheduler->feedbackDeadlineSerial = 0;
  scheduler->feedbackSamples        = 0;
  scheduler->feedbackDirty          = false;
  scheduler->readyFrameSerial       = 0;
  scheduler->readyGeneration        = 0;
  scheduler->readyScheduleEpoch     = 0;
  scheduler->readyDeadlineSerial    = 0;
  scheduler->readyTime              = 0;
}

void lgFrameSchedulerInit(LGFrameScheduler * scheduler, bool supported,
    uint32_t clientID)
{
  memset(scheduler, 0, sizeof(*scheduler));
  scheduler->supported         = supported;
  scheduler->immediatePending  = supported;
  scheduler->clientID          = clientID;
  scheduler->controlRetryDelay = FRAME_SCHEDULER_CONTROL_RETRY_NS;
}

void lgFrameSchedulerSetActive(LGFrameScheduler * scheduler, bool active)
{
  if (!scheduler->supported || scheduler->sourceActive == active)
    return;

  scheduler->sourceActive = active;
  if (!active)
    scheduler->lastTick = 0;
}

void lgFrameSchedulerSetPeriod(LGFrameScheduler * scheduler,
    uint64_t period, uint64_t tickTime)
{
  if (!scheduler->supported)
    return;

  scheduler->lastTick = tickTime;
  if (period < FRAME_SCHEDULER_MIN_PERIOD_NS ||
      period > FRAME_SCHEDULER_MAX_PERIOD_NS)
  {
    scheduler->period = 0;
    return;
  }

  bool reset = !scheduler->period;
  if (!reset)
  {
    const uint64_t delta = scheduler->period > period ?
      scheduler->period - period : period - scheduler->period;
    reset = delta > scheduler->period / 20;
  }

  scheduler->period = period;
  if (!reset)
    return;

  ++scheduler->generation;
  scheduler->resetPending           = true;
  scheduler->immediatePending       = true;
  lgFrameSchedulerResetFeedback(scheduler);
}

void lgFrameSchedulerRequestImmediate(LGFrameScheduler * scheduler)
{
  if (scheduler->supported)
    scheduler->immediatePending = true;
}

void lgFrameSchedulerObserveFrame(LGFrameScheduler * scheduler,
    uint32_t frameSerial, uint32_t generation, uint32_t scheduleEpoch,
    uint32_t deadlineSerial, uint64_t readyTime)
{
  if (!generation || !scheduleEpoch || !deadlineSerial ||
      (scheduler->readyFrameSerial == frameSerial &&
        scheduler->readyGeneration == generation &&
        scheduler->readyScheduleEpoch == scheduleEpoch &&
        scheduler->readyDeadlineSerial == deadlineSerial))
    return;

  scheduler->readyFrameSerial    = frameSerial;
  scheduler->readyGeneration     = generation;
  scheduler->readyScheduleEpoch  = scheduleEpoch;
  scheduler->readyDeadlineSerial = deadlineSerial;
  scheduler->readyTime           = readyTime;
}

void lgFrameSchedulerFeedback(LGFrameScheduler * scheduler,
    uint32_t frameSerial, uint32_t generation, uint32_t scheduleEpoch,
    uint32_t deadlineSerial, uint64_t tickTime)
{
  if (!scheduler->active || generation != scheduler->generation ||
      !scheduleEpoch || !deadlineSerial ||
      (frameSerial == scheduler->feedbackFrameSerial &&
        scheduleEpoch == scheduler->feedbackScheduleEpoch &&
        deadlineSerial == scheduler->feedbackDeadlineSerial) ||
      scheduler->readyFrameSerial != frameSerial ||
      scheduler->readyGeneration != generation ||
      scheduler->readyScheduleEpoch != scheduleEpoch ||
      scheduler->readyDeadlineSerial != deadlineSerial)
    return;

  if (scheduler->feedbackScheduleEpoch &&
      scheduler->feedbackScheduleEpoch != scheduleEpoch)
  {
    scheduler->phaseError      = 0;
    scheduler->feedbackSamples = 0;
  }

  const int64_t measuredPhase = tickTime >= scheduler->readyTime ?
    (int64_t)(tickTime - scheduler->readyTime) :
    -(int64_t)(scheduler->readyTime - tickTime);
  int64_t error = measuredPhase -
    (int64_t)FRAME_SCHEDULER_TARGET_SLACK_NS;
  const int64_t period = (int64_t)scheduler->period;
  const int64_t limit = period / 2;
  if (error > limit)
    error = limit;
  else if (error < -limit)
    error = -limit;

  if (!scheduler->feedbackSamples)
    scheduler->phaseError = error;
  else
    scheduler->phaseError = (scheduler->phaseError * 7 + error) / 8;
  if (scheduler->feedbackSamples < 32)
    ++scheduler->feedbackSamples;

  scheduler->feedbackFrameSerial    = frameSerial;
  scheduler->feedbackScheduleEpoch  = scheduleEpoch;
  scheduler->feedbackDeadlineSerial = deadlineSerial;
  scheduler->feedbackDirty          = true;
}

void lgFrameSchedulerUpdate(LGFrameScheduler * scheduler,
    PLGMPClientQueue queue, uint64_t now)
{
  if (!scheduler->supported || !queue)
    return;

  if (!lgFrameSchedulerControlReady(scheduler, queue, now))
    return;

  const bool tickStale = !scheduler->lastTick ||
    now - scheduler->lastTick > FRAME_SCHEDULER_TICK_GRACE_NS;
  if (!scheduler->sourceActive || tickStale || !scheduler->period)
  {
    if (!scheduler->active)
      return;

    const KVMFRFrameSchedule message = {
      .msg.type   = KVMFR_MESSAGE_FRAME_SCHEDULE,
      .clientID   = scheduler->clientID,
      .generation = scheduler->generation,
      .flags      = KVMFR_FRAME_SCHEDULE_RELEASE,
    };

    if (!lgFrameSchedulerSend(scheduler, queue, &message, now))
      return;

    scheduler->active           = false;
    scheduler->resetPending     = false;
    scheduler->immediatePending = true;
    scheduler->period           = 0;
    scheduler->lastSend         = now;
    lgFrameSchedulerResetFeedback(scheduler);
    return;
  }

  const uint64_t interval = scheduler->feedbackDirty ?
    FRAME_SCHEDULER_FEEDBACK_NS : FRAME_SCHEDULER_RENEW_NS;
  if (scheduler->active && !scheduler->resetPending &&
      !scheduler->immediatePending &&
      now - scheduler->lastSend < interval)
    return;

  KVMFRFrameScheduleFlags flags = KVMFR_FRAME_SCHEDULE_ACTIVE;
  if (scheduler->resetPending)
    flags |= KVMFR_FRAME_SCHEDULE_RESET;
  if (scheduler->immediatePending)
    flags |= KVMFR_FRAME_SCHEDULE_IMMEDIATE;

  const uint32_t feedbackFrameSerial =
    scheduler->feedbackFrameSerial;
  const uint32_t feedbackScheduleEpoch =
    scheduler->feedbackScheduleEpoch;
  const uint32_t feedbackDeadlineSerial =
    scheduler->feedbackDeadlineSerial;
  const KVMFRFrameSchedule message = {
    .msg.type               = KVMFR_MESSAGE_FRAME_SCHEDULE,
    .clientID               = scheduler->clientID,
    .generation             = scheduler->generation,
    .flags                  = flags,
    .period                 = scheduler->period,
    .targetSlack            = FRAME_SCHEDULER_TARGET_SLACK_NS,
    .phaseError             = scheduler->phaseError,
    .feedbackFrameSerial    = feedbackFrameSerial,
    .feedbackScheduleEpoch  = feedbackScheduleEpoch,
    .feedbackDeadlineSerial = feedbackDeadlineSerial,
    .lease                  = FRAME_SCHEDULER_LEASE_MS,
  };

  if (!lgFrameSchedulerSend(scheduler, queue, &message, now))
    return;

  scheduler->active           = true;
  scheduler->resetPending     = false;
  scheduler->immediatePending = false;
  scheduler->lastSend         = now;
  if (scheduler->feedbackFrameSerial == feedbackFrameSerial &&
      scheduler->feedbackScheduleEpoch == feedbackScheduleEpoch &&
      scheduler->feedbackDeadlineSerial == feedbackDeadlineSerial)
    scheduler->feedbackDirty = false;
}
