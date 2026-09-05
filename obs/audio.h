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

#ifndef LG_OBS_AUDIO_H
#define LG_OBS_AUDIO_H

#include <obs/obs.h>

#include <LGProtocol/KVMFRAudio.h>
#include <lgmp/client.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct LGOBSAudio LGOBSAudio;

bool lgObsAudioCreate(LGOBSAudio ** result, obs_source_t * source);
void lgObsAudioDestroy(LGOBSAudio ** audio);

bool lgObsAudioConnect(LGOBSAudio * audio, PLGMPClient client,
    uint32_t clientID);
void lgObsAudioPumpPointer(LGOBSAudio * audio,
    PLGMPClientQueue pointerQueue);
void lgObsAudioHandleResponse(LGOBSAudio * audio,
    const KVMFRAudioStreamResponse * response);

void lgObsAudioSetEnabled(LGOBSAudio * audio, bool enabled);
void lgObsAudioSetActive(LGOBSAudio * audio, bool active);
void lgObsAudioDisconnect(LGOBSAudio * audio);

#endif
