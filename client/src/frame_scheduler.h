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

#ifndef _H_LG_FRAME_SCHEDULER_
#define _H_LG_FRAME_SCHEDULER_

#include <stdint.h>

#include "interface/transport.h"

void frameScheduler_init(void);
void frameScheduler_free(void);
void frameScheduler_start(LG_TransportFeatureFlags features);
void frameScheduler_stop(void);
void frameScheduler_update(void);
void frameScheduler_feedback(uint64_t frameSerial, uint32_t generation,
    uint32_t scheduleEpoch, uint64_t measuredPhase);

#endif
