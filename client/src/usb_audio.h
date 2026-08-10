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

#ifndef _H_LG_CLIENT_USB_AUDIO_
#define _H_LG_CLIENT_USB_AUDIO_

#include "interface/audio.h"
#include "usbredir.h"

#include <stddef.h>
#include <stdint.h>

#define LG_USB_AUDIO_DEFAULT_SAMPLE_RATE 48000

typedef struct LG_USBAudio LG_USBAudio;

typedef struct LG_USBAudioEventOps
{
  void (*playbackStart)(
      void * opaque, uint32_t sampleRate, uint32_t channelMask);
  void (*playbackStop)(void * opaque);

  /* Data is borrowed interleaved packed signed 24-bit PCM, little endian.
   * Channels are ordered by ascending set bits in the selected UAC channel
   * mask. */
  void (*playbackData)(void * opaque, const void * data, size_t frames);

  /* Repeated while active when the recording clock changes rate. */
  void (*recordStart)(
      void * opaque, uint32_t sampleRate, uint32_t channelMask);
  void (*recordStop)(void * opaque);
}
LG_USBAudioEventOps;

LG_USBAudio * lgUsbAudio_create(
    const LG_USBAudioEventOps * events, void * eventOpaque, bool debug);
/* Destroy the LG_USBRedir using this device before destroying the device. */
void lgUsbAudio_destroy(LG_USBAudio * audio);

/* Publish the requested source rate without touching usbredir from the audio
 * feedback thread. */
void lgUsbAudio_setFeedbackRate(LG_USBAudio * audio, double sampleRate);
bool lgUsbAudio_feedbackActive(const LG_USBAudio * audio);

/* Queue interleaved packed signed 24-bit microphone frames. sourceClock is
 * borrowed for the call and identifies the first frame when present. This may
 * be called from the audio recording thread. */
bool lgUsbAudio_recordData(
    LG_USBAudio * audio, const void * data, size_t frames,
    const LG_AudioClock * sourceClock);

/* Return the time until ISO-IN processing is needed. This must be queried on
 * the PureSpice processing thread. */
uint64_t lgUsbAudio_processDelayNs(const LG_USBAudio * audio);

const LG_USBRedirDeviceOps * lgUsbAudio_deviceOps(void);

#endif
