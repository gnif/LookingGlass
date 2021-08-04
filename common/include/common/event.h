/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

#ifndef _H_LG_COMMON_EVENT_
#define _H_LG_COMMON_EVENT_

#include <stdbool.h>
#include <time.h>

#define TIMEOUT_INFINITE ((unsigned int)~0)

typedef struct LGEvent LGEvent;

LGEvent * lgCreateEvent(bool autoReset, unsigned int msSpinTime);
void      lgFreeEvent  (LGEvent * handle);
bool      lgWaitEvent  (LGEvent * handle, unsigned int timeout);
bool      lgSignalEvent(LGEvent * handle);
bool      lgResetEvent (LGEvent * handle);

// os specific method to wrap/convert a native event into a LGEvent
// for windows this is an event HANDLE
LGEvent * lgWrapEvent(void * handle);

// Posix specific, not implmented/possible in windows
bool lgWaitEventAbs(LGEvent * handle, struct timespec * ts);
bool lgWaitEventNS (LGEvent * handle, unsigned int timeout);

#endif
