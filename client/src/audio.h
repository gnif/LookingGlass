/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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

#if ENABLE_AUDIO

#include <stdbool.h>
#include <purespice.h>

void audio_init(void);
void audio_free(void);

bool audio_supportsPlayback(void);
void audio_playbackStart(int channels, int sampleRate, PSAudioFormat format,
  uint32_t time);
void audio_playbackStop(void);
void audio_playbackVolume(int channels, const uint16_t volume[]);
void audio_playbackMute(bool mute);
void audio_playbackData(uint8_t * data, size_t size);

bool audio_supportsRecord(void);
void audio_recordStart(int channels, int sampleRate, PSAudioFormat format);
void audio_recordToggleKeybind(int sc, void * opaque);
void audio_recordStop(void);
void audio_recordVolume(int channels, const uint16_t volume[]);
void audio_recordMute(bool mute);

#else

static inline void audio_init(void) {}
static inline void audio_free(void) {}

#endif
