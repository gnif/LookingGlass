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

#ifndef _H_I_AUDIODEV_
#define _H_I_AUDIODEV_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "interface/audio.h"

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
    /* Setup the stream for playback but don't start it yet, returning false
     * if the stream could not be configured. The pull function returns the
     * exact interleaved format supplied here. If backend resampling is
     * requested, resamplerEnabled reports whether it was activated for this
     * stream. */
    bool (*setup)(const LG_AudioFormat * format, int requestedPeriodFrames,
      bool requestResampler, bool * resamplerEnabled,
      int * maxPeriodFrames, int * startFrames, LG_AudioPullFn pullFn);

    /* called when there is data available to start playback */
    void (*start)(void);

    /* called when the source reports the audio stream has stopped */
    void (*stop)(void);

    /* [optional] called to set the volume of the channels */
    void (*volume)(int channels, const uint16_t volume[]);

    /* [optional] called to set muting of the output */
    void (*mute)(bool mute);

    /* [optional] update the active backend resampler's output/input ratio.
     * The backend replaces ratio with the value it selected for the next
     * pull. Called from the backend's playback callback and must be realtime
     * safe. */
    bool (*setRate)(double * ratio);

    /* return the current total playback latency in microseconds */
    uint64_t (*latency)(void);
  }
  playback;

  struct
  {
    /* start the record stream using the requested interleaved format */
    void (*start)(const LG_AudioFormat * format, LG_AudioPushFn pushFn);

    /* called when the source reports the audio stream has stopped */
    void (*stop)(void);

    /* [optional] called to set the volume of the channels */
    void (*volume)(int channels, const uint16_t volume[]);

    /* [optional] called to set muting of the input */
    void (*mute)(bool mute);
  }
  record;
};

#endif
