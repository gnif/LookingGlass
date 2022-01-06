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

#include "main.h"
#include "dynamic/audiodev.h"

#include <string.h>

typedef struct
{
  struct LG_AudioDevOps * audioDev;

  struct
  {
    bool       started;
    int        volumeChannels;
    uint16_t * volume;
    bool       mute;
  }
  playback;
}
AudioState;

static AudioState audio = { 0 };

void audio_init(void)
{
  // search for the best audiodev to use
  for(int i = 0; i < LG_AUDIODEV_COUNT; ++i)
    if (LG_AudioDevs[i]->init())
    {
      audio.audioDev = LG_AudioDevs[i];
      DEBUG_INFO("Using AudioDev: %s", audio.audioDev->name);
      return;
    }

  DEBUG_WARN("Failed to initialize an audio backend");
}

void audio_free(void)
{
  if (!audio.audioDev)
    return;

  audio.audioDev->free();
  audio.audioDev = NULL;

  free(audio.playback.volume);
  audio.playback.volume = NULL;
}

void audio_playbackStart(int channels, int sampleRate, PSAudioFormat format,
  uint32_t time)
{
  if (!audio.audioDev)
    return;

  static int lastChannels   = 0;
  static int lastSampleRate = 0;

  if (audio.playback.started)
  {
    if (channels != lastChannels || sampleRate != lastSampleRate)
      audio.audioDev->playback.stop();
    else
      return;
  }

  lastChannels   = channels;
  lastSampleRate = sampleRate;
  audio.playback.started = true;

  DEBUG_INFO("%d channels @ %dHz", channels, sampleRate);
  audio.audioDev->playback.start(channels, sampleRate);

  // if a volume level was stored, set it before we return
  if (audio.playback.volume)
    audio.audioDev->playback.volume(
        audio.playback.volumeChannels,
        audio.playback.volume);

  // set the inital mute state
  audio.audioDev->playback.mute(audio.playback.mute);
}

void audio_playbackStop(void)
{
  if (!audio.audioDev || !audio.playback.started)
    return;

  audio.audioDev->playback.stop();
  audio.playback.started = false;
}

void audio_playbackVolume(int channels, const uint16_t volume[])
{
  if (!audio.audioDev || !audio.audioDev->playback.volume)
    return;

  // if playback has not started yet, store the volume levels for later
  if (!audio.playback.started)
  {
    if (audio.playback.volumeChannels < channels)
    {
      free(audio.playback.volume);
      audio.playback.volume = malloc(sizeof(uint16_t) * channels);
    }
    memcpy(audio.playback.volume, volume, sizeof(uint16_t) * channels);
    audio.playback.volumeChannels = channels;
    return;
  }

  audio.audioDev->playback.volume(channels, volume);
}

void audio_playbackMute(bool mute)
{
  if (!audio.audioDev || !audio.audioDev->playback.mute)
    return;

  // if playback has not yet started, store the mute status for later
  if (!audio.playback.started)
  {
    audio.playback.mute = mute;
    return;
  }

  audio.audioDev->playback.mute(mute);
}

void audio_playbackData(uint8_t * data, size_t size)
{
  if (!audio.audioDev || !audio.playback.started)
    return;

  audio.audioDev->playback.play(data, size);
}
