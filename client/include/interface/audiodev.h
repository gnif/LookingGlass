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

#ifndef _H_I_AUDIODEV_
#define _H_I_AUDIODEV_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef int  (*LG_AudioPullFn)(uint8_t * dst, int frames);
typedef void (*LG_AudioPushFn)(uint8_t * src, int frames);

struct LG_AudioDevOps
{
  /* internal name of the audio for debugging */
  const char * name;

  /* called very early to allow for option registration, optional */
  void (*earlyInit)(void);

  /* called to initialize the audio backend */
  bool (*init)(void);

  /* final free */
  void (*free)(void);

  struct
  {
    /* setup the stream for playback but don't start it yet
     * Note: the pull function returns f32 samples
     */
    void (*setup)(int channels, int sampleRate, int requestedPeriodFrames,
      int * maxPeriodFrames, int * startFrames, LG_AudioPullFn pullFn);

    /* called when there is data available to start playback */
    void (*start)(void);

    /* called when SPICE reports the audio stream has stopped */
    void (*stop)(void);

    /* [optional] called to set the volume of the channels */
    void (*volume)(int channels, const uint16_t volume[]);

    /* [optional] called to set muting of the output */
    void (*mute)(bool mute);

    /* return the current total playback latency in microseconds */
    uint64_t (*latency)(void);
  }
  playback;

  struct
  {
    /* start the record stream
     * Note: currently SPICE only supports S16 samples so always assume so
     */
    void (*start)(int channels, int sampleRate, LG_AudioPushFn pushFn);

    /* called when SPICE reports the audio stream has stopped */
    void (*stop)(void);

    /* [optional] called to set the volume of the channels */
    void (*volume)(int channels, const uint16_t volume[]);

    /* [optional] called to set muting of the input */
    void (*mute)(bool mute);
  }
  record;
};

#endif
