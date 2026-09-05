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

#ifndef _H_LG_CLIENT_TRANSPORT_LGMP_AUDIO_
#define _H_LG_CLIENT_TRANSPORT_LGMP_AUDIO_

#include "interface/audio.h"

#include <LGProtocol/KVMFRAudio.h>
#include "common/event.h"

#include <lgmp/client.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct LGMPAudio LGMPAudio;

bool lgmpAudio_create(PLGMPClient client, LGEvent * pointerWake,
    LGMPAudio ** result);
void lgmpAudio_destroy(LGMPAudio ** audio);

bool lgmpAudio_connect(LGMPAudio * audio, uint32_t clientID);
void lgmpAudio_disconnect(LGMPAudio * audio);

/* Called only while the transport owns the existing pointer subscription. */
void lgmpAudio_pumpClaim(LGMPAudio * audio,
    PLGMPClientQueue pointerQueue);

/* The caller must copy the response and release its pointer message first. */
void lgmpAudio_handleResponse(LGMPAudio * audio,
    const KVMFRAudioStreamResponse * response);

const LG_AudioOps * lgmpAudio_getOps(void);

#endif
