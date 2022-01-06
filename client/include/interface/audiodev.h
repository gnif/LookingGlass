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
    /* start the playback audio stream
     * Note: currently SPICE only supports S16 samples so always assume so
     */
    void (*start)(int channels, int sampleRate);

    /* called for each packet of output audio to play
     * Note: size is the size of data in bytes, not frames/samples
     */
    void (*play)(uint8_t * data, int size);

    /* called when SPICE reports the audio stream has stopped */
    void (*stop)(void);

    /* [optional] called to set the volume of the channels */
    void (*volume)(int channels, const uint16_t volume[]);

    /* [optional] called to set muting of the output */
    void (*mute)(bool mute);
  }
  playback;

  struct
  {
    /* start the record stream
     * Note: currently SPICE only supports S16 samples so always assume so
     */
    void (*start)(int channels, int sampleRate,
        void (*dataFn)(uint8_t * data, int size));

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
