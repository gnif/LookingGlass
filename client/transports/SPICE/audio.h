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

#ifndef _H_LG_CLIENT_TRANSPORT_SPICE_AUDIO_
#define _H_LG_CLIENT_TRANSPORT_SPICE_AUDIO_

#include "interface/audio.h"

#include <purespice.h>

typedef struct SpiceAudio SpiceAudio;

bool spiceAudio_init(SpiceAudio ** audio);
void spiceAudio_free(SpiceAudio ** audio);

const LG_AudioOps * spiceAudio_getOps(void);
void spiceAudio_setAvailable(SpiceAudio * audio, bool available);

void spiceAudio_playbackStart(SpiceAudio * audio,
    int channels, int sampleRate,
    PSAudioFormat format, uint32_t time);
void spiceAudio_playbackStop(SpiceAudio * audio);
void spiceAudio_playbackVolume(SpiceAudio * audio,
    int channels, const uint16_t volume[]);
void spiceAudio_playbackMute(SpiceAudio * audio, bool mute);
void spiceAudio_playbackData(SpiceAudio * audio,
    uint8_t * data, size_t size, uint32_t time);

void spiceAudio_recordStart(SpiceAudio * audio,
    int channels, int sampleRate,
    PSAudioFormat format);
void spiceAudio_recordStop(SpiceAudio * audio);
void spiceAudio_recordVolume(SpiceAudio * audio,
    int channels, const uint16_t volume[]);
void spiceAudio_recordMute(SpiceAudio * audio, bool mute);

#endif
