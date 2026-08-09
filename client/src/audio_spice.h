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

#ifndef _H_LG_CLIENT_AUDIO_SPICE_
#define _H_LG_CLIENT_AUDIO_SPICE_

#include "interface/audio.h"

#include <purespice.h>

extern const LG_AudioOps LGA_Spice;

void lgaSpice_setAvailable(bool available);

void lgaSpice_playbackStart(int channels, int sampleRate,
    PSAudioFormat format, uint32_t time);
void lgaSpice_playbackStop(void);
void lgaSpice_playbackVolume(int channels, const uint16_t volume[]);
void lgaSpice_playbackMute(bool mute);
void lgaSpice_playbackData(uint8_t * data, size_t size, uint32_t time);

void lgaSpice_recordStart(int channels, int sampleRate,
    PSAudioFormat format);
void lgaSpice_recordStop(void);
void lgaSpice_recordVolume(int channels, const uint16_t volume[]);
void lgaSpice_recordMute(bool mute);

#endif
