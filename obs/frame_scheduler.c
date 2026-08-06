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

#define FRAME_SCHEDULER_LEASE_MS        1000U
#define FRAME_SCHEDULER_RENEW_NS        250000000ULL
#define FRAME_SCHEDULER_FEEDBACK_NS     50000000ULL
#define FRAME_SCHEDULER_TARGET_SLACK_NS 1000000ULL
#define FRAME_SCHEDULER_MIN_PERIOD_NS   2000000ULL
#define FRAME_SCHEDULER_MAX_PERIOD_NS   1000000000ULL

void lgFrameSchedulerInit(LGFrameScheduler * scheduler, bool supported,
    uint32_t clientID)
{
  memset(scheduler, 0, sizeof(*scheduler));
  scheduler->supported        = supported;
  scheduler->immediatePending = supported;
  scheduler->clientID         = clientID;
}

void lgFrameSchedulerSetPeriod(LGFrameScheduler * scheduler,
    uint64_t period)
{
  if (!scheduler->supported ||
      period < FRAME_SCHEDULER_MIN_PERIOD_NS ||
      period > FRAME_SCHEDULER_MAX_PERIOD_NS)
    return;

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
  scheduler->phaseError             = 0;
  scheduler->feedbackFrameSerial    = 0;
  scheduler->feedbackScheduleEpoch  = 0;
  scheduler->feedbackDeadlineSerial = 0;
  scheduler->feedbackSamples        = 0;
  scheduler->feedbackDirty          = false;
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
  if (!scheduler->supported || !scheduler->period || !queue)
    return;

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

  if (lgmpClientSendData(queue, &message, sizeof(message), NULL) != LGMP_OK)
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
