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

#ifndef LG_OBS_FRAME_SCHEDULER_H
#define LG_OBS_FRAME_SCHEDULER_H

#include <lgmp/client.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct LGFrameScheduler
{
  bool     supported;
  bool     active;
  bool     resetPending;
  bool     feedbackDirty;
  uint32_t clientID;
  uint32_t generation;
  uint64_t period;
  uint64_t lastSend;

  int64_t  phaseError;
  uint32_t feedbackFrameSerial;
  unsigned feedbackSamples;

  uint32_t readyFrameSerial;
  uint32_t readyGeneration;
  uint64_t readyTime;
}
LGFrameScheduler;

void lgFrameSchedulerInit(LGFrameScheduler * scheduler, bool supported,
    uint32_t clientID);
void lgFrameSchedulerSetPeriod(LGFrameScheduler * scheduler,
    uint64_t period);
void lgFrameSchedulerObserveFrame(LGFrameScheduler * scheduler,
    uint32_t frameSerial, uint32_t generation, uint64_t readyTime);
void lgFrameSchedulerFeedback(LGFrameScheduler * scheduler,
    uint32_t frameSerial, uint32_t generation, uint64_t neededTime);
void lgFrameSchedulerUpdate(LGFrameScheduler * scheduler,
    PLGMPClientQueue queue, uint64_t now);

#endif
